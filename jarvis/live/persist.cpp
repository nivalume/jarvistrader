#include "jarvis/live/persist.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "jarvis/live/spsc_ring.hpp"

namespace jarvis::live {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

constexpr std::size_t kMaxRecordBytes =
    wire::kRecordHeaderSize + wire::kMaxPayload + wire::kRecordTrailerSize;
// The ring must hold the largest record (SpscByteRing takes records up to half its size).
constexpr std::size_t kMinRingBytes = std::size_t{4} << 20U;
constexpr std::size_t kBatch = 4096; // records per pass before the persist thread looks at syncing
constexpr int kSpins = 64;           // idle passes that only yield before the thread sleeps
constexpr std::size_t kSnapshotsWaiting = 2;

std::uint64_t load(const std::atomic<std::uint64_t>& a) {
  return a.load(std::memory_order_relaxed);
}

} // namespace

struct Persister::Impl {
  explicit Impl(const PersistConfig& c)
      : ring{std::max(c.ring_bytes, kMinRingBytes)}, scratch(kMaxRecordBytes), config{c} {
    config.log.durable = true;
  }

  struct WaitingSnapshot {
    std::uint64_t position = 0;
    node::SnapshotInfo info;
    std::vector<std::byte> body;
  };

  SpscByteRing ring;
  std::thread thread;
  std::atomic<std::uint64_t> durable{0};

  // The core's.
  std::uint64_t position = 0;
  std::atomic<std::uint64_t> records{0};
  std::atomic<std::uint64_t> stalls{0};
  std::atomic<std::uint64_t> position_seen{0}; // position, for stats from other threads

  // The persist thread's.
  std::atomic<std::uint64_t> syncs{0};
  std::atomic<std::uint64_t> max_lag{0};
  std::atomic<std::uint64_t> segments{0};
  std::atomic<std::uint64_t> snapshots{0};
  std::atomic<std::uint64_t> snapshots_dropped{0};

  std::vector<std::byte> scratch; // the core's encoding buffer
  PersistConfig config;
  std::mutex snapshot_mutex;
  std::deque<WaitingSnapshot> waiting; // guarded by snapshot_mutex
  node::EventLogWriter writer;
  std::atomic<bool> closing{false};
  std::atomic<bool> failed{false};
  Status error = Status::Ok; // the persist thread's; read after join
  std::atomic<bool> has_waiting{false};

  void fail(Status s) {
    error = s;
    failed.store(true, std::memory_order_release);
  }

  // Writes the waiting snapshots whose position is durable (all of them with `all`).
  bool write_snapshots(std::uint64_t synced, bool all) {
    while (has_waiting.load(std::memory_order_acquire)) {
      WaitingSnapshot next;
      {
        const std::lock_guard lock{snapshot_mutex};
        if (waiting.empty() || (!all && waiting.front().position > synced)) {
          return true;
        }
        next = std::move(waiting.front());
        waiting.pop_front();
        has_waiting.store(!waiting.empty(), std::memory_order_release);
      }
      const Status s = writer.write_snapshot(next.info, next.body);
      if (!core::ok(s)) {
        fail(s);
        return false;
      }
      snapshots.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }

  // Takes up to kBatch records; false after a failed write.
  bool take(std::uint64_t& written, std::size_t& moved) {
    while (moved < kBatch) {
      bool empty = false;
      const std::span<const std::byte> record = ring.peek(empty);
      if (empty) {
        break;
      }
      const Status s = writer.append_record(record);
      ring.release();
      if (!core::ok(s)) {
        fail(s);
        return false;
      }
      written += record.size();
      ++moved;
    }
    segments.store(writer.segment_index() + 1U, std::memory_order_relaxed);
    return true;
  }

  bool sync(std::uint64_t written, std::uint64_t& synced) {
    const Status s = writer.sync();
    if (!core::ok(s)) {
      fail(s);
      return false;
    }
    const std::uint64_t lag = written - synced;
    if (lag > load(max_lag)) {
      max_lag.store(lag, std::memory_order_relaxed);
    }
    synced = written;
    durable.store(synced, std::memory_order_release);
    syncs.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void run() {
    using Clock = std::chrono::steady_clock;
    std::uint64_t written = 0;
    std::uint64_t synced = 0;
    Clock::time_point last_sync = Clock::now();
    int idle = 0;
    while (true) {
      // Everything the core appended before it asked to close is in the ring once this is seen.
      const bool closing_now = closing.load(std::memory_order_acquire);
      std::size_t moved = 0;
      if (!take(written, moved)) {
        return;
      }
      const Clock::time_point now = Clock::now();
      const bool due = config.barrier || config.sync_every.count() == 0 ||
                       now - last_sync >= config.sync_every || closing_now;
      if (written > synced && due) {
        if (!sync(written, synced)) {
          return;
        }
        last_sync = now;
      }
      if (!write_snapshots(synced, false)) {
        return;
      }
      if (closing_now && moved == 0) {
        break;
      }
      if (moved > 0) {
        idle = 0;
      } else if (++idle < kSpins) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(config.barrier ? std::chrono::microseconds{20}
                                                   : std::chrono::microseconds{500});
      }
    }
    if (!write_snapshots(synced, true)) {
      return;
    }
    const Status s = writer.close();
    if (!core::ok(s)) {
      fail(s);
      return;
    }
    durable.store(written, std::memory_order_release);
  }
};

PersistConfig persist_config(const node::NodeConfig& config) {
  PersistConfig out;
  out.barrier = config.persistence.mode == node::PersistenceMode::Barrier;
  out.sync_every = std::chrono::milliseconds{config.persistence.sync_every_ms};
  return out;
}

Persister::Persister(const PersistConfig& config) : impl_{std::make_unique<Impl>(config)} {}

Persister::~Persister() { static_cast<void>(close()); }

Status Persister::open(const std::string& directory, const wire::LogHeader& header,
                       std::string& error) {
  Impl& p = *impl_;
  if (p.thread.joinable()) {
    return Status::InvalidState;
  }
  const Status s = p.writer.open(directory, header, p.config.log);
  if (!core::ok(s)) {
    error = "cannot open the run log in " + directory + ": " + std::string{core::to_string(s)};
    return s;
  }
  p.thread = std::thread{[&p] { p.run(); }};
  return Status::Ok;
}

Status Persister::append(const core::EventKey& key, const model::Event& event) {
  std::size_t written = 0;
  const Status s = wire::encode_record(key, event, impl_->scratch, written);
  if (!core::ok(s)) {
    return s;
  }
  return append_record(std::span<const std::byte>{impl_->scratch.data(), written});
}

Status Persister::append_output(const core::EventKey& key, const model::Output& output) {
  std::size_t written = 0;
  const Status s = wire::encode_output_record(key, output, impl_->scratch, written);
  if (!core::ok(s)) {
    return s;
  }
  return append_record(std::span<const std::byte>{impl_->scratch.data(), written});
}

Status Persister::append_record(std::span<const std::byte> record) {
  Impl& p = *impl_;
  if (!p.thread.joinable() || p.closing.load(std::memory_order_relaxed)) {
    return Status::InvalidState;
  }
  if (!p.ring.try_write(record)) {
    p.stalls.fetch_add(1, std::memory_order_relaxed);
    do {
      if (p.failed.load(std::memory_order_acquire)) {
        return Status::IoError;
      }
      std::this_thread::yield();
    } while (!p.ring.try_write(record));
  }
  if (p.failed.load(std::memory_order_acquire)) {
    return Status::IoError;
  }
  p.position += record.size();
  p.position_seen.store(p.position, std::memory_order_relaxed);
  p.records.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok;
}

Status Persister::write_snapshot(const node::SnapshotInfo& info, std::vector<std::byte> body) {
  Impl& p = *impl_;
  if (!p.thread.joinable() || p.closing.load(std::memory_order_relaxed)) {
    return Status::InvalidState;
  }
  if (p.failed.load(std::memory_order_acquire)) {
    return Status::IoError;
  }
  const std::lock_guard lock{p.snapshot_mutex};
  if (p.waiting.size() >= kSnapshotsWaiting) {
    p.waiting.pop_front();
    p.snapshots_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  p.waiting.push_back(Impl::WaitingSnapshot{p.position, info, std::move(body)});
  p.has_waiting.store(true, std::memory_order_release);
  return Status::Ok;
}

std::uint64_t Persister::position() const noexcept { return impl_->position; }

const std::atomic<std::uint64_t>& Persister::durable() const noexcept { return impl_->durable; }

bool Persister::failed() const noexcept { return impl_->failed.load(std::memory_order_acquire); }

PersistStats Persister::stats() const noexcept {
  const Impl& p = *impl_;
  return PersistStats{load(p.records),  load(p.position_seen), load(p.durable),
                      load(p.syncs),    load(p.stalls),        load(p.max_lag),
                      load(p.segments), load(p.snapshots),     load(p.snapshots_dropped)};
}

Status Persister::close() {
  Impl& p = *impl_;
  if (!p.thread.joinable()) {
    return p.error;
  }
  p.closing.store(true, std::memory_order_release);
  p.thread.join();
  return p.error;
}

} // namespace jarvis::live
