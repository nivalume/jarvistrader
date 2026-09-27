#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/risk/rate_limit.hpp"

// Order book synchronization of one USDⓈ-M depth stream (docs/architecture.md section 14.3), the
// object of specs/tla/DepthSync.tla. No I/O: the adapter feeds it connection changes, decoded
// depth diffs and REST snapshots, asks it whether a snapshot is wanted, and forwards what it
// emits to the kernel.
//
//   Idle        not connected
//   Buffering   connected; diffs are buffered; a snapshot is wanted (wants_snapshot())
//   Requested   a snapshot request is in flight; diffs are still buffered
//   Validating  the snapshot is the local book; waiting for the diff with U <= L <= u
//   Synced      every diff must have pu equal to the previous u; the kernel sees the book
//
// When the snapshot arrives, buffered diffs with u < L are dropped; the first of the rest must
// cover L, and the rest must chain. A stale snapshot (the first kept diff starts after L), a
// break in the chain, or a disconnect sends the book back to Buffering (Idle for a disconnect).
//
// Output (OrderBookDeltas through the emitter): on entering Synced a CLEAR followed by every
// level as Add, all flagged F_SNAPSHOT and the last F_LAST; in sync one batch per diff (Update,
// or Delete for size zero; the last F_LAST); on leaving Synced a lone CLEAR, so the kernel never
// holds a book that is out of sync. The sequence of a batch is the last applied update id.

namespace jarvis::adapter::binance {

enum class DepthSyncPhase : std::uint8_t { Idle, Buffering, Requested, Validating, Synced };

struct DepthSyncConfig {
  // Diffs kept while waiting for a snapshot; on overflow the buffer is discarded (the
  // snapshot then proves stale and is requested again).
  std::size_t max_buffer = 4096;
  // Levels kept per side, from the touch out (0: no limit). The diff stream also updates
  // levels far from the touch, so an unbounded book only grows; levels beyond the snapshot's
  // depth (1000) are partial knowledge anyway. The worst levels are dropped, and in sync the
  // drop is emitted as Delete so the kernel's book stays equal to this one. Every level kept
  // has the venue's size; a level beyond the snapshot's depth or dropped here stays unknown
  // until it changes, so after a move through many levels the far side of the book can miss
  // levels (a limit of the venue's snapshot size, not of the synchronizer).
  std::size_t max_levels = 2000;
};

// A REST depth snapshot (GET /fapi/v1/depth).
struct DepthSnapshot {
  std::uint64_t last_update_id = 0;
  core::UnixNanos ts_event; // T of the response
  core::UnixNanos ts_init;  // arrival
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

struct DepthSyncStats {
  std::uint64_t syncs = 0;           // entries into Synced
  std::uint64_t gaps = 0;            // chain breaks in sync or in the buffer
  std::uint64_t stale_snapshots = 0; // snapshots older than the buffered stream
  std::uint64_t dropped_diffs = 0;   // buffered diffs older than the snapshot
  std::uint64_t overflows = 0;       // buffer discarded
  std::uint64_t applied = 0;         // diffs applied in sync
  std::uint64_t trimmed = 0;         // levels dropped beyond max_levels
};

class DepthSync {
public:
  using Bids = std::map<std::int64_t, BookLevel, std::greater<>>; // by price raw, best first
  using Asks = std::map<std::int64_t, BookLevel, std::less<>>;

  explicit DepthSync(model::InstrumentId id, DepthSyncConfig config = {})
      : id_{id}, config_{config} {}

  [[nodiscard]] DepthSyncPhase phase() const noexcept { return phase_; }
  [[nodiscard]] bool visible() const noexcept { return phase_ == DepthSyncPhase::Synced; }
  // The last applied update id (the snapshot's L while Validating; 0 before).
  [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_u_; }
  [[nodiscard]] const Bids& bids() const noexcept { return bids_; }
  [[nodiscard]] const Asks& asks() const noexcept { return asks_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
  [[nodiscard]] const DepthSyncStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const model::InstrumentId& instrument_id() const noexcept { return id_; }

  // The stream connected (or reconnected): start buffering.
  void connected() {
    if (phase_ == DepthSyncPhase::Idle) {
      phase_ = DepthSyncPhase::Buffering;
    }
  }

  // The stream dropped: forget everything; a CLEAR if the kernel had the book.
  [[nodiscard]] core::Status disconnected(core::UnixNanos ts, EventEmitter& out) {
    const bool was_synced = phase_ == DepthSyncPhase::Synced;
    reset(DepthSyncPhase::Idle);
    ++request_; // an answer to an earlier request is ignored
    return was_synced ? emit_clear(ts, out) : core::Status::Ok;
  }

  [[nodiscard]] bool wants_snapshot() const noexcept { return phase_ == DepthSyncPhase::Buffering; }

  // The adapter sent the snapshot request; the returned id must come back with the answer.
  [[nodiscard]] std::uint64_t snapshot_requested() {
    if (phase_ == DepthSyncPhase::Buffering) {
      phase_ = DepthSyncPhase::Requested;
    }
    return ++request_;
  }

  // The request failed (HTTP error, timeout): want a snapshot again.
  void snapshot_failed(std::uint64_t request) {
    if (phase_ == DepthSyncPhase::Requested && request == request_) {
      phase_ = DepthSyncPhase::Buffering;
    }
  }

  [[nodiscard]] core::Status on_diff(const DepthDiff& d, EventEmitter& out) {
    switch (phase_) {
    case DepthSyncPhase::Idle:
      return core::Status::InvalidState;
    case DepthSyncPhase::Buffering:
    case DepthSyncPhase::Requested:
      buffer(d);
      return core::Status::Ok;
    case DepthSyncPhase::Validating:
      if (d.final_update_id < last_u_) {
        ++stats_.dropped_diffs;
        return core::Status::Ok;
      }
      if (d.first_update_id <= last_u_) {
        apply(d.bids, d.asks, d.final_update_id);
        return enter_synced(d.ts_event, d.ts_init, out);
      }
      ++stats_.stale_snapshots;
      resync_with(d);
      return core::Status::Ok;
    case DepthSyncPhase::Synced:
      if (d.prev_final_update_id != last_u_) {
        ++stats_.gaps;
        const core::UnixNanos ts = d.ts_init;
        resync_with(d);
        return emit_clear(ts, out);
      }
      apply(d.bids, d.asks, d.final_update_id);
      ++stats_.applied;
      return emit_diff(d, out);
    }
    return core::Status::InvalidState;
  }

  // The snapshot answering request `request`; answers to other requests are ignored.
  [[nodiscard]] core::Status on_snapshot(std::uint64_t request, const DepthSnapshot& s,
                                         EventEmitter& out) {
    if (phase_ != DepthSyncPhase::Requested || request != request_) {
      return core::Status::Ok;
    }
    const std::uint64_t l = s.last_update_id;
    while (!buffer_.empty() && buffer_.front().final_update_id < l) {
      buffer_.pop_front();
      ++stats_.dropped_diffs;
    }
    clear_book();
    last_u_ = l;
    if (buffer_.empty()) {
      apply(s.bids, s.asks, l);
      phase_ = DepthSyncPhase::Validating;
      return core::Status::Ok;
    }
    if (buffer_.front().first_update_id > l) {
      ++stats_.stale_snapshots;
      last_u_ = 0;
      phase_ = DepthSyncPhase::Buffering; // keep the buffer for the next snapshot
      return core::Status::Ok;
    }
    apply(s.bids, s.asks, l);
    bool first = true;
    while (!buffer_.empty()) {
      const Buffered& b = buffer_.front();
      if (!first && b.prev_final_update_id != last_u_) {
        ++stats_.gaps;
        clear_book();
        last_u_ = 0;
        phase_ = DepthSyncPhase::Buffering; // the buffer from the break on stays
        return core::Status::Ok;
      }
      apply(b.bids, b.asks, b.final_update_id);
      first = false;
      buffer_.pop_front();
    }
    return enter_synced(s.ts_event, s.ts_init, out);
  }

private:
  struct Buffered {
    std::uint64_t first_update_id = 0;
    std::uint64_t final_update_id = 0;
    std::uint64_t prev_final_update_id = 0;
    core::UnixNanos ts_event;
    core::UnixNanos ts_init;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
  };

  void buffer(const DepthDiff& d) {
    if (buffer_.size() >= config_.max_buffer) {
      buffer_.clear();
      ++stats_.overflows;
    }
    buffer_.push_back(Buffered{d.first_update_id,
                               d.final_update_id,
                               d.prev_final_update_id,
                               d.ts_event,
                               d.ts_init,
                               {d.bids.begin(), d.bids.end()},
                               {d.asks.begin(), d.asks.end()}});
  }

  void resync_with(const DepthDiff& d) {
    reset(DepthSyncPhase::Buffering);
    buffer(d);
  }

  void reset(DepthSyncPhase phase) {
    phase_ = phase;
    clear_book();
    last_u_ = 0;
    buffer_.clear();
  }

  void clear_book() {
    bids_.clear();
    asks_.clear();
    trimmed_.clear();
  }

  template <typename Map> static void set_level(Map& side, const BookLevel& level) {
    if (level.size.is_zero()) {
      side.erase(level.price.raw()); // removing an absent level is normal
    } else {
      side[level.price.raw()] = level;
    }
  }

  void apply(std::span<const BookLevel> bids, std::span<const BookLevel> asks, std::uint64_t u) {
    for (const BookLevel& l : bids) {
      set_level(bids_, l);
    }
    for (const BookLevel& l : asks) {
      set_level(asks_, l);
    }
    last_u_ = u;
    trim(bids_, model::OrderSide::Buy);
    trim(asks_, model::OrderSide::Sell);
  }

  // Drops the worst levels beyond max_levels, remembering them for the next diff batch.
  template <typename Map> void trim(Map& side, model::OrderSide which) {
    while (config_.max_levels > 0 && side.size() > config_.max_levels) {
      auto worst = std::prev(side.end());
      BookLevel gone = worst->second;
      gone.size = model::Quantity{};
      trimmed_.push_back({which, gone});
      side.erase(worst);
      ++stats_.trimmed;
    }
  }

  static model::OrderBookDelta delta(const model::InstrumentId& id, model::BookAction action,
                                     std::optional<model::OrderSide> side, const BookLevel& l,
                                     std::uint8_t flags, std::uint64_t sequence,
                                     core::UnixNanos ts_event, core::UnixNanos ts_init) {
    return model::OrderBookDelta{
        id, action, model::BookOrder{side, l.price, l.size, 0}, flags, sequence, ts_event, ts_init};
  }

  core::Status emit(EventEmitter& out) {
    if (deltas_.empty()) {
      return core::Status::Ok;
    }
    deltas_.back().flags |= model::flag_bit(model::RecordFlag::F_LAST);
    model::OrderBookDeltas batch;
    const core::Status s = model::OrderBookDeltas::create(deltas_, batch);
    return core::ok(s) ? out.event(batch) : s;
  }

  core::Status enter_synced(core::UnixNanos ts_event, core::UnixNanos ts_init, EventEmitter& out) {
    trimmed_.clear(); // the snapshot batch lists only the levels kept
    phase_ = DepthSyncPhase::Synced;
    ++stats_.syncs;
    const std::uint8_t snap = model::flag_bit(model::RecordFlag::F_SNAPSHOT);
    deltas_.clear();
    deltas_.push_back(delta(id_, model::BookAction::Clear, std::nullopt, BookLevel{}, snap, last_u_,
                            ts_event, ts_init));
    for (const auto& [raw, l] : bids_) {
      deltas_.push_back(delta(id_, model::BookAction::Add, model::OrderSide::Buy, l, snap, last_u_,
                              ts_event, ts_init));
    }
    for (const auto& [raw, l] : asks_) {
      deltas_.push_back(delta(id_, model::BookAction::Add, model::OrderSide::Sell, l, snap, last_u_,
                              ts_event, ts_init));
    }
    return emit(out);
  }

  core::Status emit_diff(const DepthDiff& d, EventEmitter& out) {
    deltas_.clear();
    const auto add = [&](const BookLevel& l, model::OrderSide side) {
      deltas_.push_back(
          delta(id_, l.size.is_zero() ? model::BookAction::Delete : model::BookAction::Update, side,
                l, 0, d.final_update_id, d.ts_event, d.ts_init));
    };
    for (const BookLevel& l : d.bids) {
      add(l, model::OrderSide::Buy);
    }
    for (const BookLevel& l : d.asks) {
      add(l, model::OrderSide::Sell);
    }
    for (const auto& [side, l] : trimmed_) {
      add(l, side);
    }
    trimmed_.clear();
    return emit(out);
  }

  core::Status emit_clear(core::UnixNanos ts, EventEmitter& out) {
    deltas_.clear();
    deltas_.push_back(
        delta(id_, model::BookAction::Clear, std::nullopt, BookLevel{}, 0, 0, ts, ts));
    return emit(out);
  }

  model::InstrumentId id_;
  DepthSyncConfig config_;
  DepthSyncPhase phase_ = DepthSyncPhase::Idle;
  std::uint64_t last_u_ = 0;
  std::uint64_t request_ = 0;
  Bids bids_;
  Asks asks_;
  std::deque<Buffered> buffer_;
  std::vector<model::OrderBookDelta> deltas_;
  std::vector<std::pair<model::OrderSide, BookLevel>> trimmed_;
  DepthSyncStats stats_;
};

// Snapshot requests cost REST weight (20 of the IP's 2400 a minute for 1000 levels). They share
// one budget per adapter so a burst of gaps across symbols cannot exhaust the weight: by
// default at most 5 in any 10-second window and 20 in any minute (fixed windows, like the
// venue's own counters; risk::RateLimiter).
inline constexpr std::array<risk::RateWindow, 2> kDefaultSnapshotWindows{
    {{10'000'000'000ULL, 5}, {60'000'000'000ULL, 20}}};

struct SnapshotRequest {
  std::uint32_t symbol = 0; // SymbolTable index
  std::uint64_t request = 0;
};

// The depth books of one adapter, one DepthSync per symbol of the table.
class DepthBooks {
public:
  DepthBooks(const SymbolTable& symbols, DepthSyncConfig config = {},
             std::span<const risk::RateWindow> snapshot_windows = kDefaultSnapshotWindows)
      : limiter_{snapshot_windows} {
    books_.reserve(symbols.size());
    for (std::uint32_t i = 0; i < symbols.size(); ++i) {
      books_.emplace_back(symbols[i].id, config);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return books_.size(); }
  [[nodiscard]] DepthSync& operator[](std::uint32_t symbol) { return books_.at(symbol); }
  [[nodiscard]] const DepthSync& operator[](std::uint32_t symbol) const {
    return books_.at(symbol);
  }

  void connected() {
    for (DepthSync& b : books_) {
      b.connected();
    }
  }

  [[nodiscard]] core::Status disconnected(core::UnixNanos ts, EventEmitter& out) {
    core::Status first = core::Status::Ok;
    for (DepthSync& b : books_) {
      const core::Status s = b.disconnected(ts, out);
      first = core::ok(first) ? s : first;
    }
    return first;
  }

  [[nodiscard]] core::Status on_diff(const DepthDiff& d, EventEmitter& out) {
    return d.symbol < books_.size() ? books_[d.symbol].on_diff(d, out) : core::Status::OutOfRange;
  }

  // The snapshot requests to send now, within the budget; books are taken in turn from where
  // the previous call stopped, so one symbol's gaps cannot starve the others.
  void due_snapshots(core::UnixNanos now, std::vector<SnapshotRequest>& out) {
    out.clear();
    for (std::size_t n = 0; n < books_.size(); ++n) {
      const std::size_t i = (next_ + n) % books_.size();
      if (!books_[i].wants_snapshot()) {
        continue;
      }
      if (!limiter_.try_acquire(now)) {
        next_ = i;
        return;
      }
      out.push_back(SnapshotRequest{static_cast<std::uint32_t>(i), books_[i].snapshot_requested()});
    }
  }

  [[nodiscard]] core::Status on_snapshot(const SnapshotRequest& r, const DepthSnapshot& s,
                                         EventEmitter& out) {
    return r.symbol < books_.size() ? books_[r.symbol].on_snapshot(r.request, s, out)
                                    : core::Status::OutOfRange;
  }

  void snapshot_failed(const SnapshotRequest& r) {
    if (r.symbol < books_.size()) {
      books_[r.symbol].snapshot_failed(r.request);
    }
  }

private:
  std::vector<DepthSync> books_;
  risk::RateLimiter limiter_;
  std::size_t next_ = 0;
};

} // namespace jarvis::adapter::binance
