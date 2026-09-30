// The persist thread (jarvis/live/persist.hpp): the log it writes is the one EventLogWriter
// writes on the core thread, byte for byte; the durable position follows the configured mode;
// a busy ring loses nothing. Run under the tsan preset too.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include <doctest/doctest.h>

#include "jarvis/live/persist.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/corpus.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/snapshot_file.hpp"

namespace {

namespace live = jarvis::live;
namespace m = jarvis::model;
namespace node = jarvis::node;
namespace wire = jarvis::model::wire;
using jarvis::core::EventKey;
using jarvis::core::Status;

class TempDir {
public:
  explicit TempDir(std::string_view name) {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("jarvis-persist-" + std::string{name} + "-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter++));
    std::filesystem::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  [[nodiscard]] std::string str() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

wire::LogHeader header() {
  wire::LogHeader h;
  h.seed = 9;
  h.config_hash[0] = 0x5A;
  return h;
}

std::vector<std::string> files(const std::string& dir) {
  std::vector<std::string> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    out.push_back(entry.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<char> bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// Appends `n` corpus events (seed 21) to `writer` (EventLogWriter or Persister).
template <typename W> void append_corpus(W& writer, std::uint64_t n) {
  node::CorpusGenerator corpus{21};
  EventKey key;
  m::Event event;
  for (std::uint64_t i = 0; i < n; ++i) {
    REQUIRE(corpus.next(key, event) == Status::Ok);
    REQUIRE(writer.append(key, event) == Status::Ok);
  }
}

std::uint64_t count_records(const std::string& dir) {
  node::EventLogReader reader;
  REQUIRE(reader.open(dir) == Status::Ok);
  wire::RecordView record;
  std::uint64_t n = 0;
  Status s = Status::Ok;
  while ((s = reader.next(record)) == Status::Ok) {
    ++n;
  }
  CHECK(s == Status::EndOfStream);
  return n;
}

bool wait_durable(const live::Persister& p, std::uint64_t position) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < end) {
    if (p.durable().load() >= position) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the persist thread writes the log EventLogWriter writes, segments included") {
    const TempDir a{"writer"};
    const TempDir b{"persist"};
    node::EventLogOptions options;
    options.segment_bytes = 64ULL * 1024ULL;
    node::EventLogWriter writer;
    REQUIRE(writer.open(a.str(), header(), options) == Status::Ok);
    append_corpus(writer, 3000);
    REQUIRE(writer.close() == Status::Ok);

    live::PersistConfig config;
    config.log = options;
    live::Persister persister{config};
    std::string error;
    REQUIRE(persister.open(b.str(), header(), error) == Status::Ok);
    append_corpus(persister, 3000);
    CHECK(persister.position() == writer.position());
    REQUIRE(persister.close() == Status::Ok);

    const std::vector<std::string> names = files(a.str());
    REQUIRE(names.size() > 3);
    CHECK(files(b.str()) == names);
    for (const std::string& name : names) {
      CHECK(bytes(a.str() + "/" + name) == bytes(b.str() + "/" + name));
    }
    const live::PersistStats stats = persister.stats();
    CHECK(stats.records == 3000);
    CHECK(stats.position == writer.position());
    CHECK(stats.durable == writer.position());
    CHECK(stats.segments == names.size());
    CHECK(persister.durable().load() == writer.position());
  }

  TEST_CASE("barrier: the durable position reaches every append without closing") {
    const TempDir dir{"barrier"};
    live::PersistConfig config;
    config.barrier = true;
    config.sync_every = std::chrono::hours{1}; // barrier ignores the cadence
    live::Persister persister{config};
    std::string error;
    REQUIRE(persister.open(dir.str(), header(), error) == Status::Ok);
    append_corpus(persister, 50);
    CHECK(wait_durable(persister, persister.position()));
    const std::uint64_t first = persister.position();
    append_corpus(persister, 50);
    CHECK(persister.position() > first);
    CHECK(wait_durable(persister, persister.position()));
    CHECK(persister.stats().syncs >= 2);
    REQUIRE(persister.close() == Status::Ok);
    CHECK(count_records(dir.str()) == 100);
  }

  TEST_CASE("async: the durable position moves on the sync cadence, and at close") {
    const TempDir slow{"async-slow"};
    live::PersistConfig config;
    config.sync_every = std::chrono::hours{1};
    live::Persister persister{config};
    std::string error;
    REQUIRE(persister.open(slow.str(), header(), error) == Status::Ok);
    append_corpus(persister, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK(persister.durable().load() == 0); // written, perhaps, but never synced
    REQUIRE(persister.close() == Status::Ok);
    CHECK(persister.durable().load() == persister.position());
    CHECK(persister.stats().syncs == 1);

    const TempDir fast{"async-fast"};
    config.sync_every = std::chrono::milliseconds{0};
    live::Persister every{config};
    REQUIRE(every.open(fast.str(), header(), error) == Status::Ok);
    append_corpus(every, 200);
    CHECK(wait_durable(every, every.position()));
    REQUIRE(every.close() == Status::Ok);
  }

  TEST_CASE("a busy ring loses nothing and keeps the order") {
    const TempDir a{"busy-writer"};
    const TempDir b{"busy-persist"};
    constexpr std::uint64_t kEvents = 60'000; // several times the smallest ring
    node::EventLogWriter writer;
    REQUIRE(writer.open(a.str(), header()) == Status::Ok);
    append_corpus(writer, kEvents);
    REQUIRE(writer.close() == Status::Ok);

    live::PersistConfig config;
    config.ring_bytes = 1; // raised to the smallest ring that holds the largest record
    live::Persister persister{config};
    std::string error;
    REQUIRE(persister.open(b.str(), header(), error) == Status::Ok);
    append_corpus(persister, kEvents);
    REQUIRE(persister.close() == Status::Ok);
    CHECK(bytes(a.str() + "/events-000000.jlog") == bytes(b.str() + "/events-000000.jlog"));
    MESSAGE("ring stalls: " << persister.stats().stalls);
  }

  TEST_CASE("a snapshot file is written only once the log is durable up to it") {
    const auto snapshot_file = [](const std::string& dir, std::uint64_t seq) {
      return dir + "/" + node::snapshot_name(seq);
    };
    const std::vector<std::byte> body{std::byte{1}, std::byte{2}, std::byte{3}};

    // async, syncing once an hour: the snapshot waits for the sync at close.
    const TempDir slow{"snap-async"};
    live::PersistConfig config;
    config.sync_every = std::chrono::hours{1};
    live::Persister persister{config};
    std::string error;
    REQUIRE(persister.open(slow.str(), header(), error) == Status::Ok);
    append_corpus(persister, 100);
    REQUIRE(persister.write_snapshot(node::SnapshotInfo{100, 7, true, 1}, body) == Status::Ok);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK_FALSE(std::filesystem::exists(snapshot_file(slow.str(), 100)));
    REQUIRE(persister.close() == Status::Ok);
    REQUIRE(std::filesystem::exists(snapshot_file(slow.str(), 100)));
    std::vector<std::byte> file;
    REQUIRE(node::read_file_bytes(snapshot_file(slow.str(), 100), file) == Status::Ok);
    wire::LogHeader h;
    node::SnapshotInfo info;
    std::span<const std::byte> read;
    REQUIRE(node::decode_snapshot(file, h, info, read) == Status::Ok);
    CHECK(info.seq == 100);
    CHECK(info.ts == 7);
    CHECK(info.complete);
    CHECK(std::vector<std::byte>(read.begin(), read.end()) == body);
    CHECK(h.seed == header().seed);
    CHECK(persister.stats().snapshots == 1);

    // barrier: written as soon as the log is synced past it.
    const TempDir fast{"snap-barrier"};
    config.barrier = true;
    live::Persister barrier{config};
    REQUIRE(barrier.open(fast.str(), header(), error) == Status::Ok);
    append_corpus(barrier, 100);
    REQUIRE(barrier.write_snapshot(node::SnapshotInfo{100, 7, true, 1}, body) == Status::Ok);
    bool written = false;
    for (int i = 0; i < 5000 && !written; ++i) {
      written = std::filesystem::exists(snapshot_file(fast.str(), 100));
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK(written);
    REQUIRE(barrier.close() == Status::Ok);
  }

  TEST_CASE("at most two snapshots wait; a third replaces the older one") {
    const TempDir dir{"snap-drop"};
    live::PersistConfig config;
    config.sync_every = std::chrono::hours{1};
    live::Persister persister{config};
    std::string error;
    REQUIRE(persister.open(dir.str(), header(), error) == Status::Ok);
    append_corpus(persister, 10);
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
      REQUIRE(persister.write_snapshot(node::SnapshotInfo{seq, seq, true, 1},
                                       std::vector<std::byte>{std::byte{0}}) == Status::Ok);
    }
    REQUIRE(persister.close() == Status::Ok);
    const std::vector<node::SnapshotEntry> list = node::list_snapshots(dir.str());
    REQUIRE(list.size() == 2);
    CHECK(list[0].seq == 2);
    CHECK(list[1].seq == 3);
    CHECK(persister.stats().snapshots == 2);
    CHECK(persister.stats().snapshots_dropped == 1);
  }

  TEST_CASE("the persister refuses appends before open and after close") {
    const TempDir dir{"closed"};
    live::Persister persister{live::PersistConfig{}};
    node::CorpusGenerator corpus{1};
    EventKey key;
    m::Event event;
    REQUIRE(corpus.next(key, event) == Status::Ok);
    CHECK(persister.append(key, event) == Status::InvalidState);
    std::string error;
    REQUIRE(persister.open(dir.str(), header(), error) == Status::Ok);
    CHECK(persister.append(key, event) == Status::Ok);
    REQUIRE(persister.close() == Status::Ok);
    CHECK(persister.append(key, event) == Status::InvalidState);
    CHECK(persister.close() == Status::Ok);

    live::Persister again{live::PersistConfig{}};
    CHECK(again.open(dir.str(), header(), error) == Status::AlreadyExists);
    CHECK(error.find("cannot open the run log") != std::string::npos);
  }
}
