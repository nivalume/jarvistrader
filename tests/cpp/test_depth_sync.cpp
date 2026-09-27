// The depth synchronizer (jarvis/adapter/binance/depth_sync.hpp, specs/tla/DepthSync.tla):
// scripted transitions, the REST snapshot decoder, and a property test that plays a random
// exchange (batched updates, lost events, disconnects, stale snapshots) and checks that the
// kernel's order book, fed only with what the synchronizer emits, always equals the exchange's
// book at the last applied update whenever the book is visible.

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/rest_codec.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace adapter = jarvis::adapter;
namespace binance = jarvis::adapter::binance;
namespace model = jarvis::model;
using binance::DepthSyncPhase;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

constexpr std::uint8_t kPricePrecision = 1;
constexpr std::uint8_t kSizePrecision = 3;
constexpr std::int64_t kTickRaw = 100'000'000; // 0.1

model::InstrumentId instrument() {
  model::InstrumentId id;
  REQUIRE(binance::perpetual_id("BTCUSDT", id) == Status::Ok);
  return id;
}

// Price p (a tick index) and size q (in lots) as a level.
adapter::BookLevel level(std::int64_t p, std::uint64_t q) {
  adapter::BookLevel l;
  REQUIRE(model::Price::from_raw(1000 * 1'000'000'000LL + p * kTickRaw, kPricePrecision, l.price) ==
          Status::Ok);
  REQUIRE(model::Quantity::from_raw(q * 1'000'000, kSizePrecision, l.size) == Status::Ok);
  return l;
}

struct Diff {
  std::uint64_t U;
  std::uint64_t u;
  std::uint64_t pu;
  std::vector<adapter::BookLevel> bids;
  std::vector<adapter::BookLevel> asks;

  [[nodiscard]] adapter::DepthDiff view(std::uint64_t ts = 1) const {
    adapter::DepthDiff d;
    d.instrument_id = instrument();
    d.first_update_id = U;
    d.final_update_id = u;
    d.prev_final_update_id = pu;
    d.ts_event = UnixNanos{ts};
    d.ts_init = UnixNanos{ts};
    d.bids = bids;
    d.asks = asks;
    return d;
  }
};

binance::DepthSnapshot snapshot(std::uint64_t last, std::vector<adapter::BookLevel> bids,
                                std::vector<adapter::BookLevel> asks = {}) {
  binance::DepthSnapshot s;
  s.last_update_id = last;
  s.bids = std::move(bids);
  s.asks = std::move(asks);
  return s;
}

const model::OrderBookDeltas& batch(const adapter::CollectingEmitter& out, std::size_t i) {
  return std::get<model::OrderBookDeltas>(out.events.at(i));
}

std::map<std::int64_t, std::uint64_t> sizes(const binance::DepthSync::Bids& side) {
  std::map<std::int64_t, std::uint64_t> m;
  for (const auto& [raw, l] : side) {
    m[raw] = l.size.raw();
  }
  return m;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("buffered diffs older than the snapshot are dropped and the rest applied") {
    binance::DepthSync sync{instrument()};
    adapter::CollectingEmitter out;
    CHECK(sync.on_diff(Diff{1, 1, 0, {}, {}}.view(), out) == Status::InvalidState); // not connected
    sync.connected();
    REQUIRE(sync.wants_snapshot());
    for (const Diff& d : {Diff{1, 3, 0, {level(1, 5)}, {}}, Diff{4, 6, 3, {level(2, 7)}, {}},
                          Diff{7, 9, 6, {level(1, 0)}, {level(5, 2)}}}) {
      REQUIRE(sync.on_diff(d.view(), out) == Status::Ok);
    }
    const std::uint64_t req = sync.snapshot_requested();
    CHECK(sync.phase() == DepthSyncPhase::Requested);
    REQUIRE(sync.on_snapshot(req, snapshot(5, {level(1, 4), level(3, 1)}), out) == Status::Ok);
    CHECK(sync.phase() == DepthSyncPhase::Synced);
    CHECK(sync.last_update_id() == 9);
    CHECK(sync.stats().dropped_diffs == 1);
    // Snapshot {1:4, 3:1}, then u=6 sets 2:7, then u=9 removes 1 and adds ask 5:2.
    CHECK(sizes(sync.bids()) ==
          std::map<std::int64_t, std::uint64_t>{{level(3, 1).price.raw(), 1'000'000},
                                                {level(2, 7).price.raw(), 7'000'000}});
    REQUIRE(out.events.size() == 1);
    const model::OrderBookDeltas& snap = batch(out, 0);
    REQUIRE(snap.deltas.size() == 4); // CLEAR, two bids, one ask
    CHECK(snap.deltas[0].action == model::BookAction::Clear);
    CHECK(snap.deltas[1].action == model::BookAction::Add);
    CHECK(snap.deltas[1].order.price == level(3, 0).price); // best bid first
    CHECK(model::has_flag(snap.deltas[3].flags, model::RecordFlag::F_SNAPSHOT));
    CHECK(model::has_flag(snap.deltas[3].flags, model::RecordFlag::F_LAST));
    CHECK_FALSE(model::has_flag(snap.deltas[2].flags, model::RecordFlag::F_LAST));
    CHECK(snap.sequence == 9);

    // In sync: one batch per diff, zero sizes are deletes.
    REQUIRE(sync.on_diff(Diff{10, 11, 9, {level(2, 0), level(4, 3)}, {}}.view(), out) ==
            Status::Ok);
    const model::OrderBookDeltas& d = batch(out, 1);
    REQUIRE(d.deltas.size() == 2);
    CHECK(d.deltas[0].action == model::BookAction::Delete);
    CHECK(d.deltas[1].action == model::BookAction::Update);
    CHECK(model::has_flag(d.deltas[1].flags, model::RecordFlag::F_LAST));
    CHECK(d.sequence == 11);
  }

  TEST_CASE("a snapshot newer than the buffer waits for the diff that covers it") {
    binance::DepthSync sync{instrument()};
    adapter::CollectingEmitter out;
    sync.connected();
    REQUIRE(sync.on_diff(Diff{1, 3, 0, {level(1, 1)}, {}}.view(), out) == Status::Ok);
    const std::uint64_t req = sync.snapshot_requested();
    REQUIRE(sync.on_snapshot(req, snapshot(5, {level(1, 2)}), out) == Status::Ok);
    CHECK(sync.phase() == DepthSyncPhase::Validating);
    CHECK(sync.last_update_id() == 5);
    CHECK_FALSE(sync.visible());
    REQUIRE(sync.on_diff(Diff{4, 4, 3, {level(9, 9)}, {}}.view(), out) == Status::Ok); // u < L
    CHECK(sync.phase() == DepthSyncPhase::Validating);
    REQUIRE(sync.on_diff(Diff{5, 6, 4, {level(2, 1)}, {}}.view(), out) == Status::Ok);
    CHECK(sync.phase() == DepthSyncPhase::Synced);
    CHECK(out.events.size() == 1);

    // Validating and the next diff starts after L: the snapshot is stale.
    binance::DepthSync other{instrument()};
    other.connected();
    const std::uint64_t r2 = other.snapshot_requested();
    REQUIRE(other.on_snapshot(r2, snapshot(5, {}), out) == Status::Ok);
    REQUIRE(other.on_diff(Diff{7, 8, 6, {}, {}}.view(), out) == Status::Ok);
    CHECK(other.phase() == DepthSyncPhase::Buffering);
    CHECK(other.buffered() == 1);
    CHECK(other.stats().stale_snapshots == 1);
  }

  TEST_CASE("a stale snapshot or a gap in the buffer asks for a new snapshot") {
    adapter::CollectingEmitter out;
    binance::DepthSync stale{instrument()};
    stale.connected();
    REQUIRE(stale.on_diff(Diff{8, 9, 7, {}, {}}.view(), out) == Status::Ok);
    std::uint64_t req = stale.snapshot_requested();
    REQUIRE(stale.on_snapshot(req, snapshot(5, {}), out) == Status::Ok); // first kept U=8 > 5
    CHECK(stale.phase() == DepthSyncPhase::Buffering);
    CHECK(stale.wants_snapshot());
    CHECK(stale.buffered() == 1); // kept for the next snapshot
    req = stale.snapshot_requested();
    REQUIRE(stale.on_snapshot(req, snapshot(8, {}), out) == Status::Ok);
    CHECK(stale.phase() == DepthSyncPhase::Synced);

    binance::DepthSync gap{instrument()};
    gap.connected();
    for (const Diff& d :
         {Diff{4, 6, 3, {}, {}}, Diff{9, 10, 8, {}, {}}, Diff{11, 12, 10, {}, {}}}) {
      REQUIRE(gap.on_diff(d.view(), out) == Status::Ok);
    }
    req = gap.snapshot_requested();
    REQUIRE(gap.on_snapshot(req, snapshot(5, {}), out) == Status::Ok); // 6 -> 9 breaks (pu 8)
    CHECK(gap.phase() == DepthSyncPhase::Buffering);
    CHECK(gap.buffered() == 2);
    CHECK(gap.stats().gaps == 1);
    CHECK(out.events.size() == 1); // only the stale case's sync; the gap case never showed a book
  }

  TEST_CASE("a gap or a disconnect in sync clears the kernel's book") {
    binance::DepthSync sync{instrument()};
    adapter::CollectingEmitter out;
    sync.connected();
    REQUIRE(sync.on_diff(Diff{5, 6, 4, {level(1, 1)}, {}}.view(), out) == Status::Ok);
    std::uint64_t req = sync.snapshot_requested();
    REQUIRE(sync.on_snapshot(req, snapshot(5, {}), out) == Status::Ok);
    REQUIRE(sync.phase() == DepthSyncPhase::Synced);
    REQUIRE(sync.on_diff(Diff{9, 9, 8, {}, {}}.view(), out) == Status::Ok); // pu 8 != 6
    CHECK(sync.phase() == DepthSyncPhase::Buffering);
    REQUIRE(out.events.size() == 2);
    const model::OrderBookDeltas& clear = batch(out, 1);
    REQUIRE(clear.deltas.size() == 1);
    CHECK(clear.deltas[0].action == model::BookAction::Clear);
    CHECK(model::has_flag(clear.deltas[0].flags, model::RecordFlag::F_LAST));

    req = sync.snapshot_requested();
    REQUIRE(sync.disconnected(UnixNanos{50}, out) == Status::Ok); // not in sync: no CLEAR
    CHECK(out.events.size() == 2);
    CHECK(sync.phase() == DepthSyncPhase::Idle);
    sync.connected();
    REQUIRE(sync.on_snapshot(req, snapshot(9, {}), out) == Status::Ok); // an old answer
    CHECK(sync.phase() == DepthSyncPhase::Buffering);
    const std::uint64_t again = sync.snapshot_requested();
    sync.snapshot_failed(again);
    CHECK(sync.wants_snapshot());
  }

  TEST_CASE("an overflowing buffer is discarded") {
    binance::DepthSync sync{instrument(), binance::DepthSyncConfig{3}};
    adapter::CollectingEmitter out;
    sync.connected();
    for (std::uint64_t i = 1; i <= 4; ++i) {
      REQUIRE(sync.on_diff(Diff{i, i, i - 1, {}, {}}.view(), out) == Status::Ok);
    }
    CHECK(sync.buffered() == 1);
    CHECK(sync.stats().overflows == 1);
  }

  TEST_CASE("levels beyond max_levels are dropped and the kernel is told") {
    binance::DepthSyncConfig cfg;
    cfg.max_levels = 3;
    binance::DepthSync sync{instrument(), cfg};
    adapter::CollectingEmitter out;
    sync.connected();
    REQUIRE(sync.on_diff(Diff{5, 5, 4, {}, {}}.view(), out) == Status::Ok);
    const std::uint64_t req = sync.snapshot_requested();
    REQUIRE(sync.on_snapshot(req, snapshot(5, {level(9, 1), level(8, 1), level(7, 1), level(6, 1)}),
                             out) == Status::Ok);
    REQUIRE(sync.phase() == DepthSyncPhase::Synced);
    CHECK(sync.bids().size() == 3);          // 6 dropped
    CHECK(batch(out, 0).deltas.size() == 4); // CLEAR and the three kept levels
    // A better bid pushes 7 out: the batch carries its Delete.
    REQUIRE(sync.on_diff(Diff{6, 6, 5, {level(10, 2)}, {}}.view(), out) == Status::Ok);
    const model::OrderBookDeltas& d = batch(out, 1);
    REQUIRE(d.deltas.size() == 2);
    CHECK(d.deltas[1].action == model::BookAction::Delete);
    CHECK(d.deltas[1].order.price == level(7, 0).price);
    CHECK(sync.stats().trimmed == 2);
  }

  TEST_CASE("snapshot requests share one budget and take symbols in turn") {
    adapter::SymbolTable table;
    for (const char* s : {"AUSDT", "BUSDT", "CUSDT"}) {
      model::InstrumentId id;
      REQUIRE(binance::perpetual_id(s, id) == Status::Ok);
      REQUIRE(table.add(s, adapter::SymbolEntry{id, 1, 3, {}}) == Status::Ok);
    }
    const std::array<jarvis::risk::RateWindow, 1> windows{{{10'000'000'000ULL, 2}}};
    binance::DepthBooks books{table, {}, windows};
    books.connected();
    std::vector<binance::SnapshotRequest> due;
    books.due_snapshots(UnixNanos{1}, due);
    REQUIRE(due.size() == 2); // the budget allows two in this window
    CHECK(due[0].symbol == 0);
    CHECK(due[1].symbol == 1);
    books.due_snapshots(UnixNanos{2}, due);
    CHECK(due.empty());
    books.due_snapshots(UnixNanos{10'000'000'001ULL}, due); // the next window
    REQUIRE(due.size() == 1);
    CHECK(due[0].symbol == 2);
    adapter::CollectingEmitter out;
    REQUIRE(books.on_snapshot(due[0], snapshot(1, {}), out) == Status::Ok);
    CHECK(books[2].phase() == DepthSyncPhase::Validating);
  }

  TEST_CASE("the REST depth snapshot decodes exactly") {
    std::ifstream in{std::string{JARVIS_SOURCE_DIR} +
                     "/tests/data/binance/depth_snapshot_testnet.json"};
    std::ostringstream json;
    json << in.rdbuf();
    const adapter::SymbolEntry btc{instrument(), 1, 4, {}};
    binance::DepthSnapshot s;
    std::string error;
    REQUIRE(binance::decode_depth_snapshot(json.str(), btc, UnixNanos{3}, s, error) == Status::Ok);
    CHECK(s.last_update_id == 439000670774ULL);
    CHECK(s.ts_event == UnixNanos{1790528827370ULL * 1'000'000});
    REQUIRE(s.bids.size() == 5);
    REQUIRE(s.asks.size() == 5);
    CHECK(s.bids[0].price.raw() == 84432'700'000'000LL);
    CHECK(s.bids[0].size.raw() == 513'400'000ULL);
    const adapter::SymbolEntry coarse{instrument(), 0, 4, {}}; // 84432.7 is off a whole-dollar grid
    CHECK(binance::decode_depth_snapshot(json.str(), coarse, UnixNanos{3}, s, error) ==
          Status::PrecisionLoss);
    CHECK(binance::decode_depth_snapshot("{\"bids\":[]}", btc, UnixNanos{3}, s, error) ==
          Status::ParseError);

    // The same snapshot through the WebSocket API's `depth` method.
    const std::string ws = R"({"id":"d7","status":200,"result":)" + json.str() +
                           R"(,"rateLimits":[{"rateLimitType":"REQUEST_WEIGHT","count":7}]})";
    std::string id;
    binance::DepthSnapshot w;
    REQUIRE(binance::decode_ws_depth_response(ws, btc, UnixNanos{4}, id, w, error) == Status::Ok);
    CHECK(id == "d7");
    CHECK(w.last_update_id == s.last_update_id);
    CHECK(w.bids.size() == 5);
    CHECK(binance::decode_ws_depth_response(
              R"({"id":"d8","status":400,"error":{"code":-1121,"msg":"Invalid symbol."}})", btc,
              UnixNanos{4}, id, w, error) == Status::InvalidArgument);
    CHECK(id == "d8");
    CHECK(error == "status 400: Invalid symbol.");
  }
}

TEST_SUITE("property") {
  TEST_CASE("the kernel's book, fed by the synchronizer, equals the exchange's book") {
    // max_levels 0 keeps every level: the kernel's book is the exchange's. With a cap of 4 the
    // kernel's book is the synchronizer's and every level in it has the exchange's size, but a
    // level dropped while better ones existed stays unknown once they go (the same limit a
    // 1000-level snapshot has in production), so levels can be missing.
    for (const std::size_t cap : {std::size_t{0}, std::size_t{4}}) {
      jarvis::testkit::for_all([cap](jarvis::testkit::Gen& gen) {
        // The exchange: sizes by tick, numbered updates, events of accumulated changes.
        constexpr std::int64_t kTicks = 12;
        std::map<std::int64_t, std::uint64_t> xbook;
        std::vector<std::map<std::int64_t, std::uint64_t>> hist{xbook};
        std::map<std::int64_t, std::uint64_t> pending;
        std::uint64_t pending_first = 0;
        std::uint64_t last_pub = 0;
        bool connected = false;
        std::vector<Diff> channel;

        binance::DepthSyncConfig config;
        config.max_levels = cap;
        binance::DepthSync sync{instrument(), config};
        std::optional<std::pair<std::uint64_t, binance::DepthSnapshot>> inflight;
        jarvis::data::BookConfig cfg;
        REQUIRE(model::Price::from_raw(kTickRaw, kPricePrecision, cfg.tick) == Status::Ok);
        cfg.size_precision = kSizePrecision;
        cfg.window_levels = 256;
        cfg.overflow_levels = 64;
        jarvis::data::OrderBook kernel{cfg};
        adapter::CollectingEmitter out;

        const auto feed_kernel = [&] {
          for (const model::Event& e : out.events) {
            REQUIRE(kernel.apply(std::get<model::OrderBookDeltas>(e)) == Status::Ok);
          }
          out.clear();
        };
        const auto kernel_sizes = [&] {
          std::map<std::int64_t, std::uint64_t> m;
          std::array<jarvis::data::BookLevel, 64> levels{};
          const std::size_t n = kernel.bids(levels);
          for (std::size_t i = 0; i < n; ++i) {
            m[(levels[i].price.raw() - 1000 * 1'000'000'000LL) / kTickRaw] =
                levels[i].size.raw() / 1'000'000;
          }
          return m;
        };

        for (int step = 0; step < 300; ++step) {
          const std::uint64_t pick = gen.below(100);
          if (pick < 35 && hist.size() < 200) { // an exchange update
            const std::int64_t p = gen.range(0, kTicks - 1);
            const std::uint64_t q = gen.below(4);
            if (q == 0) {
              xbook.erase(p);
            } else {
              xbook[p] = q;
            }
            if (pending.empty()) {
              pending_first = hist.size();
            }
            pending[p] = q;
            hist.push_back(xbook);
          } else if (pick < 55 && !pending.empty()) { // publish the accumulated changes
            Diff d{pending_first, hist.size() - 1, last_pub, {}, {}};
            for (const auto& [p, q] : pending) {
              d.bids.push_back(level(p, q));
            }
            last_pub = hist.size() - 1;
            pending.clear();
            if (connected) {
              channel.push_back(d);
            }
          } else if (pick < 58 && !channel.empty()) { // lose an event
            channel.erase(channel.begin());
          } else if (pick < 80 && !channel.empty()) { // deliver an event
            REQUIRE(sync.on_diff(channel.front().view(), out) == Status::Ok);
            channel.erase(channel.begin());
          } else if (pick < 83) { // (dis)connect
            if (connected) {
              connected = false;
              channel.clear();
              inflight.reset();
              REQUIRE(sync.disconnected(UnixNanos{1}, out) == Status::Ok);
            } else {
              connected = true;
              sync.connected();
            }
          } else if (pick < 90 && sync.wants_snapshot()) {
            const std::uint64_t req = sync.snapshot_requested();
            // Served at the current update or, now and then, at an older one (a stale replica).
            const std::uint64_t at = gen.chance(1, 4) ? gen.below(hist.size()) : hist.size() - 1;
            binance::DepthSnapshot s;
            s.last_update_id = at;
            for (const auto& [p, q] : hist[at]) {
              s.bids.push_back(level(p, q));
            }
            inflight = std::pair{req, s};
          } else if (inflight) { // the snapshot arrives
            REQUIRE(sync.on_snapshot(inflight->first, inflight->second, out) == Status::Ok);
            inflight.reset();
          }
          const bool had_output = !out.events.empty();
          feed_kernel();
          if (sync.visible()) {
            REQUIRE(sync.last_update_id() < hist.size());
            const std::map<std::int64_t, std::uint64_t>& exchange = hist[sync.last_update_id()];
            const std::map<std::int64_t, std::uint64_t> seen = kernel_sizes();
            if (cap == 0) {
              CHECK(seen == exchange);
            } else {
              std::map<std::int64_t, std::uint64_t> local;
              for (const auto& [raw, l] : sync.bids()) {
                local[(raw - 1000 * 1'000'000'000LL) / kTickRaw] = l.size.raw() / 1'000'000;
              }
              CHECK(seen == local);
              CHECK(seen.size() <= cap);
              for (const auto& [p, q] : seen) {
                const auto it = exchange.find(p);
                CHECK((it != exchange.end() && it->second == q));
              }
            }
          } else if (had_output) {
            CHECK(kernel_sizes().empty()); // left sync: the kernel's book is cleared
          }
        }
      });
    }
  }
}
