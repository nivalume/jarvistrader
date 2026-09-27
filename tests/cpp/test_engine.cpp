#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/testkit/alloc.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace e = jarvis::engine;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::model::LifecycleReason;
using jarvis::model::NodeState;
using jarvis::testkit::Gen;

constexpr std::array<NodeState, e::kNodeStateCount> kStates = {
    NodeState::Init,     NodeState::Wired,   NodeState::Starting,
    NodeState::Syncing,  NodeState::Running, NodeState::Degraded,
    NodeState::Stopping, NodeState::Stopped, NodeState::Faulted};
constexpr std::array<LifecycleReason, e::kLifecycleReasonCount> kReasons = {
    LifecycleReason::Configured, LifecycleReason::RunRequested,
    LifecycleReason::Started,    LifecycleReason::Synced,
    LifecycleReason::HealthLost, LifecycleReason::HealthRestored,
    LifecycleReason::EndOfData,  LifecycleReason::ShutdownRequested,
    LifecycleReason::Drained,    LifecycleReason::Fault};

NodeState run(std::initializer_list<LifecycleReason> reasons) {
  e::Lifecycle machine;
  jarvis::model::NodeLifecycle event;
  std::uint64_t ts = 1;
  for (const LifecycleReason r : reasons) {
    REQUIRE(machine.apply(r, UnixNanos{ts++}, event) == Status::Ok);
  }
  return machine.state();
}

// States reachable from `start` through the transition table.
std::set<NodeState> reachable(NodeState start) {
  std::set<NodeState> seen{start};
  std::vector<NodeState> frontier{start};
  while (!frontier.empty()) {
    const NodeState s = frontier.back();
    frontier.pop_back();
    for (const e::LifecycleTransition& t : e::kLifecycleTransitions) {
      if (t.from == s && seen.insert(t.to).second) {
        frontier.push_back(t.to);
      }
    }
  }
  return seen;
}

namespace st = jarvis::strategy;
namespace dt = jarvis::data;
namespace md = jarvis::model;
using jarvis::core::EventKey;
using jarvis::testkit::AllocationScope;

md::InstrumentId iid(std::string_view text) {
  md::InstrumentId id;
  REQUIRE(md::InstrumentId::parse(text, id) == Status::Ok);
  return id;
}
md::Price price(std::string_view text) {
  md::Price p;
  REQUIRE(md::Price::parse(text, p) == Status::Ok);
  return p;
}
md::Quantity quantity(std::string_view text) {
  md::Quantity q;
  REQUIRE(md::Quantity::parse(text, q) == Status::Ok);
  return q;
}
md::Event trade_at(std::uint64_t ts, std::string_view px, std::string_view size = "1.000") {
  md::TradeTick t;
  t.instrument_id = iid("BTCUSDT-PERP.BINANCE");
  t.price = price(px);
  t.size = quantity(size);
  t.aggressor_side = md::AggressorSide::Buy;
  REQUIRE(md::TradeId::from(std::to_string(ts), t.trade_id) == Status::Ok);
  t.ts_event = UnixNanos{ts};
  t.ts_init = UnixNanos{ts};
  return md::Event{t};
}
md::Event quote_at(std::uint64_t ts, std::string_view bid, std::string_view ask) {
  md::QuoteTick q;
  q.instrument_id = iid("BTCUSDT-PERP.BINANCE");
  q.bid_price = price(bid);
  q.ask_price = price(ask);
  q.bid_size = quantity("1.000");
  q.ask_size = quantity("2.000");
  q.ts_event = UnixNanos{ts};
  q.ts_init = UnixNanos{ts};
  return md::Event{q};
}
md::Event running(std::uint64_t ts) {
  return md::Event{md::NodeLifecycle{NodeState::Syncing, NodeState::Running,
                                     LifecycleReason::Synced, UnixNanos{ts}}};
}
md::Event batch_end(std::uint64_t ts) { return md::Event{md::BatchEnd{0, UnixNanos{ts}}}; }

// Records every callback as text. Subscriptions are chosen by `plan`.
// Test strategies write through pointers to state owned by the test; real strategies mutate
// their own members, so the callbacks stay non-const as they would be there.
// NOLINTBEGIN(readability-make-member-function-const)
struct Recorder {
  std::string plan; // "trades:every", "quotes:conflated", ...
  std::vector<std::string>* log = nullptr;
  Status fail_on_trade = Status::Ok;
  md::FeatureId feature = 0;

  Status on_start(st::Context& ctx) {
    const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
    log->push_back("start");
    if (plan == "trades:every") {
      return ctx.subscribe_trades(btc);
    }
    if (plan == "quotes:conflated") {
      return ctx.subscribe_quotes(btc, dt::Cadence::conflated());
    }
    if (plan == "trades:sampled") {
      return ctx.subscribe_trades(btc, dt::Cadence::sampled_ns(1'000'000'000));
    }
    if (plan == "trades:batch") {
      return ctx.subscribe_trades(btc, dt::Cadence::on_batch());
    }
    if (plan == "timer") {
      return ctx.set_timer(7, UnixNanos{ctx.now().value() + 100}, jarvis::core::DurationNanos{50});
    }
    if (plan == "bars") {
      md::BarType type;
      REQUIRE(md::BarType::parse("BTCUSDT-PERP.BINANCE-1-SECOND-LAST-INTERNAL", type) ==
              Status::Ok);
      return ctx.subscribe_bars(type);
    }
    if (plan == "feature") {
      return ctx.feature(dt::FeatureSpec{dt::FeatureKind::Ema, btc, 3}, dt::Cadence::every(),
                         feature);
    }
    if (plan == "book") {
      return ctx.subscribe_book(btc, dt::Cadence::every(), md::BookType::L1_MBP);
    }
    return Status::Ok;
  }
  Status on_trade(st::Context& ctx, const md::TradeTick& t) {
    log->push_back("trade " + std::to_string(t.ts_init.value()));
    if (plan == "trades:every") {
      static_cast<void>(ctx.record("last", md::Decimal{}));
    }
    return fail_on_trade;
  }
  Status on_quote(st::Context& /*ctx*/, const md::QuoteTick& q) {
    log->push_back("quote " + std::to_string(q.ts_init.value()));
    return Status::Ok;
  }
  Status on_trade_batch(st::Context& /*ctx*/, const st::TradeBatch& b) {
    log->push_back("batch " + std::to_string(b.trades.size()));
    return Status::Ok;
  }
  Status on_timer(st::Context& /*ctx*/, jarvis::core::TimerKey key, UnixNanos deadline) {
    log->push_back("timer " + std::to_string(key.id) + " " + std::to_string(deadline.value()));
    return Status::Ok;
  }
  Status on_bar(st::Context& /*ctx*/, const md::Bar& bar) {
    log->push_back("bar " + std::to_string(bar.ts_init.value()) + " close " +
                   std::to_string(bar.close.raw()));
    return Status::Ok;
  }
  Status on_feature(st::Context& /*ctx*/, md::FeatureId id, md::Decimal value, UnixNanos /*ts*/) {
    log->push_back("feature " + std::to_string(id) + " " + std::to_string(value.raw()));
    return Status::Ok;
  }
  Status on_book(st::Context& /*ctx*/, const dt::BookView& book) {
    dt::BookLevel best;
    REQUIRE(book.best_bid(best));
    log->push_back("book " + std::to_string(best.price.raw()));
    return Status::Ok;
  }
  Status on_error(st::Context& /*ctx*/, const md::StrategyError& /*e*/) {
    log->push_back("error");
    return Status::Ok;
  }
  Status on_stop(st::Context& /*ctx*/) {
    log->push_back("stop");
    return Status::Ok;
  }
};
// NOLINTEND(readability-make-member-function-const)

st::KernelConfig small_config() {
  st::KernelConfig c;
  c.instruments = 8;
  c.strategies = 4;
  c.timers = 16;
  c.batch = 3;
  c.features = 4;
  c.bar_types = 4;
  c.buffers = 16;
  c.book_window_levels = 256;
  c.book_overflow_levels = 64;
  c.trading.orders = 256;
  c.trading.trades = 1024;
  return c;
}

// Feeds events like the node does: due timers become TimerFired inputs first.
template <typename E>
Status drive(E& engine, const std::vector<md::Event>& events, std::uint64_t& seq) {
  for (const md::Event& event : events) {
    const UnixNanos ts = md::ts_init_of(event);
    jarvis::core::FiredTimer due;
    while (engine.next_timer(due) && due.deadline <= ts) {
      const md::TimerFired fired{due.key, due.deadline, due.deadline};
      const Status s = engine.step(EventKey{due.deadline, 9, ++seq}, md::Event{fired});
      if (!jarvis::core::ok(s)) {
        return s;
      }
    }
    const Status s = engine.step(EventKey{ts, 0, ++seq}, event);
    if (!jarvis::core::ok(s)) {
      return s;
    }
  }
  return Status::Ok;
}

std::vector<std::string> run_plan(std::string_view plan, const std::vector<md::Event>& events) {
  std::vector<std::string> log;
  st::StaticStrategySet<Recorder> set{Recorder{std::string{plan}, &log}};
  jarvis::engine::Engine engine{small_config(), set};
  std::uint64_t seq = 0;
  REQUIRE(drive(engine, events, seq) == Status::Ok);
  return log;
}

using Log = std::vector<std::string>;

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the backtest path runs Init to Stopped") {
    CHECK(run({LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started,
               LifecycleReason::Synced, LifecycleReason::EndOfData, LifecycleReason::Drained}) ==
          NodeState::Stopped);
  }

  TEST_CASE("a live node degrades, resyncs and shuts down") {
    CHECK(
        run({LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started,
             LifecycleReason::Synced, LifecycleReason::HealthLost, LifecycleReason::HealthRestored,
             LifecycleReason::Synced, LifecycleReason::ShutdownRequested,
             LifecycleReason::ShutdownRequested, LifecycleReason::Drained}) == NodeState::Stopped);
  }

  TEST_CASE("each transition records a NodeLifecycle event") {
    e::Lifecycle machine;
    jarvis::model::NodeLifecycle event;
    REQUIRE(machine.apply(LifecycleReason::Configured, UnixNanos{5}, event) == Status::Ok);
    CHECK(event.from == NodeState::Init);
    CHECK(event.to == NodeState::Wired);
    CHECK(event.reason == LifecycleReason::Configured);
    CHECK(event.ts_init == UnixNanos{5});
    const jarvis::model::NodeLifecycle before = event;
    CHECK(machine.apply(LifecycleReason::Synced, UnixNanos{6}, event) == Status::InvalidTransition);
    CHECK(machine.state() == NodeState::Wired);
    CHECK(event.ts_init == before.ts_init); // untouched on failure
  }

  TEST_CASE("the transition table is a function and only terminal states are dead ends") {
    std::set<std::pair<NodeState, LifecycleReason>> keys;
    for (const e::LifecycleTransition& t : e::kLifecycleTransitions) {
      CHECK(keys.insert({t.from, t.reason}).second);
      CHECK_FALSE(e::is_terminal(t.from));
    }
    for (const NodeState s : kStates) {
      bool has_exit = false;
      for (const LifecycleReason r : kReasons) {
        NodeState to = s;
        const Status status = e::next_state(s, r, to);
        CHECK((status == Status::Ok) == keys.contains({s, r}));
        has_exit = has_exit || status == Status::Ok;
      }
      CHECK(has_exit != e::is_terminal(s));
    }
  }

  TEST_CASE("every state is reachable and every live state can reach both terminals") {
    CHECK(reachable(NodeState::Init).size() == e::kNodeStateCount);
    for (const NodeState s : kStates) {
      if (e::is_terminal(s)) {
        CHECK(reachable(s).size() == 1);
        continue;
      }
      const std::set<NodeState> r = reachable(s);
      CHECK(r.contains(NodeState::Faulted));
      if (s != NodeState::Init) {
        CHECK(r.contains(NodeState::Stopped));
      }
    }
  }

  TEST_CASE("Running is entered only from Syncing") {
    for (const e::LifecycleTransition& t : e::kLifecycleTransitions) {
      if (t.to == NodeState::Running) {
        CHECK(t.from == NodeState::Syncing);
        CHECK(t.reason == LifecycleReason::Synced);
      }
    }
    CHECK(e::strategies_active(NodeState::Running));
    CHECK_FALSE(e::strategies_active(NodeState::Syncing));
    CHECK_FALSE(e::strategies_active(NodeState::Degraded));
  }

  TEST_CASE("replay accepts recorded transitions and rejects tampered ones") {
    e::Lifecycle live;
    std::vector<jarvis::model::NodeLifecycle> log;
    for (const LifecycleReason r :
         {LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started}) {
      jarvis::model::NodeLifecycle event;
      REQUIRE(live.apply(r, UnixNanos{1}, event) == Status::Ok);
      log.push_back(event);
    }
    e::Lifecycle replayed;
    for (const auto& event : log) {
      REQUIRE(replayed.replay(event) == Status::Ok);
    }
    CHECK(replayed.state() == live.state());

    e::Lifecycle tampered;
    jarvis::model::NodeLifecycle forged = log[0];
    forged.to = NodeState::Running;
    CHECK(tampered.replay(forged) == Status::InvalidTransition);
    CHECK(tampered.state() == NodeState::Init);
  }
}

TEST_SUITE("unit") {
  TEST_CASE("strategies start on Running and receive subscribed trades in order") {
    const Log log = run_plan("trades:every", {trade_at(5, "100.0"), running(10),
                                              trade_at(20, "100.1"), trade_at(30, "100.2")});
    CHECK(log == Log{"start", "trade 20", "trade 30"}); // nothing before on_start
  }

  TEST_CASE("conflated quotes deliver the latest of each batch at BatchEnd") {
    const Log log =
        run_plan("quotes:conflated",
                 {running(1), quote_at(10, "100.0", "100.1"), quote_at(11, "100.1", "100.2"),
                  batch_end(11), batch_end(12), quote_at(13, "100.0", "100.1"), batch_end(13)});
    CHECK(log == Log{"start", "quote 11", "quote 13"});
  }

  TEST_CASE("sampled trades deliver the first update of each period") {
    const Log log =
        run_plan("trades:sampled", {running(1), trade_at(100'000'000, "1"),
                                    trade_at(500'000'000, "1"), trade_at(1'200'000'000, "1"),
                                    trade_at(1'300'000'000, "1"), trade_at(2'500'000'000, "1")});
    CHECK(log == Log{"start", "trade 100000000", "trade 1200000000", "trade 2500000000"});
  }

  TEST_CASE("OnBatch buffers deliver per batch and flush early when full") {
    const Log log = run_plan("trades:batch", {running(1), trade_at(10, "1"), trade_at(11, "1"),
                                              batch_end(11), trade_at(12, "1"), trade_at(13, "1"),
                                              trade_at(14, "1"), trade_at(15, "1"), batch_end(15)});
    CHECK(log == Log{"start", "batch 2", "batch 3", "batch 1"}); // capacity 3
  }

  TEST_CASE("strategy timers fire as recorded inputs and periodic timers re-arm") {
    const Log log = run_plan("timer", {running(1000), trade_at(1150, "1"), trade_at(1300, "1")});
    CHECK(log == Log{"start", "timer 7 1100", "timer 7 1150", "timer 7 1200", "timer 7 1250",
                     "timer 7 1300"});
  }

  TEST_CASE("internal time bars close on the kernel's timer") {
    const Log log =
        run_plan("bars", {running(1), trade_at(1'100'000'000, "100.0"),
                          trade_at(1'900'000'000, "101.0"), trade_at(2'400'000'000, "99.0")});
    CHECK(log == Log{"start", "bar 2000000000 close 101000000000"});
  }

  TEST_CASE("features are delivered and recorded as outputs") {
    std::vector<std::string> log;
    st::StaticStrategySet<Recorder> set{Recorder{"feature", &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {running(1), trade_at(10, "100.0")}, seq) == Status::Ok);
    CHECK(log == Log{"start", "feature 0 100000000000"});
    REQUIRE(engine.outputs().size() == 1);
    CHECK(std::holds_alternative<md::FeatureUpdate>(engine.outputs()[0]));
  }

  TEST_CASE("an L1 book follows quotes") {
    const Log log = run_plan(
        "book", {running(1), quote_at(10, "100.0", "100.1"), quote_at(11, "100.3", "100.4")});
    CHECK(log == Log{"start", "book 100000000000", "book 100300000000"});
  }

  TEST_CASE("a failing callback becomes a failure, then the StrategyError input halts it") {
    std::vector<std::string> log;
    Recorder r{"trades:every", &log};
    r.fail_on_trade = Status::InvalidArgument;
    st::StaticStrategySet<Recorder> set{r};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {running(1), trade_at(10, "1")}, seq) == Status::Ok);
    REQUIRE(engine.failures().size() == 1);
    const st::StrategyFailure f = engine.failures()[0];
    engine.clear_failures();
    const md::StrategyError error{f.strategy, f.kind, f.message_hash, UnixNanos{10}};
    REQUIRE(engine.step(EventKey{UnixNanos{10}, 0, ++seq}, md::Event{error}) == Status::Ok);
    REQUIRE(drive(engine, {trade_at(20, "1")}, seq) == Status::Ok);
    CHECK(log == Log{"start", "trade 10", "error"}); // no trade 20
  }

  TEST_CASE("records are outputs keyed by the causing input") {
    std::vector<std::string> log;
    st::StaticStrategySet<Recorder> set{Recorder{"trades:every", &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {running(1), trade_at(10, "1")}, seq) == Status::Ok);
    struct Sink {
      std::vector<EventKey> keys;
      Status emit(const EventKey& key, const md::Output& /*o*/) {
        keys.push_back(key);
        return Status::Ok;
      }
    } sink;
    REQUIRE(engine.flush_outputs(sink) == Status::Ok);
    REQUIRE(sink.keys.size() == 1);
    CHECK(sink.keys[0].seq == 2);
    CHECK(sink.keys[0].source_id == 0);
    CHECK(engine.outputs().empty());
  }

  TEST_CASE("a recorded timer that is not the one due is a divergence") {
    std::vector<std::string> log;
    st::StaticStrategySet<Recorder> set{Recorder{"timer", &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {running(1000)}, seq) == Status::Ok);
    const md::TimerFired wrong{jarvis::core::TimerKey{0, 8}, UnixNanos{1100}, UnixNanos{1100}};
    CHECK(engine.step(EventKey{UnixNanos{1100}, 9, ++seq}, md::Event{wrong}) ==
          Status::InvalidState);
  }

  TEST_CASE("static and dynamic strategy sets behave identically") {
    const std::vector<md::Event> events{running(1), trade_at(10, "1"), quote_at(11, "1", "2"),
                                        batch_end(11)};
    Log a;
    Log b;
    Log c;
    Log d;
    st::StaticStrategySet<Recorder, Recorder> fixed{Recorder{"trades:every", &a},
                                                    Recorder{"quotes:conflated", &b}};
    Recorder r1{"trades:every", &c};
    Recorder r2{"quotes:conflated", &d};
    st::DynamicStrategySet dynamic{2};
    REQUIRE(dynamic.add(r1) == Status::Ok);
    REQUIRE(dynamic.add(r2) == Status::Ok);
    jarvis::engine::Engine e1{small_config(), fixed};
    jarvis::engine::Engine e2{small_config(), dynamic};
    std::uint64_t s1 = 0;
    std::uint64_t s2 = 0;
    REQUIRE(drive(e1, events, s1) == Status::Ok);
    REQUIRE(drive(e2, events, s2) == Status::Ok);
    CHECK(a == c);
    CHECK(b == d);
    CHECK(a == Log{"start", "trade 10"});
    CHECK(b == Log{"start", "quote 11"});
  }
}

TEST_SUITE("property") {
  TEST_CASE("the engine is a function of its inputs") {
    jarvis::testkit::for_all([](Gen& gen) {
      std::vector<md::Event> events{running(1)};
      std::uint64_t ts = 1;
      for (int i = 0; i < 80; ++i) {
        ts += 1 + gen.below(400'000'000);
        const std::string px =
            std::to_string(100 + gen.below(5)) + "." + std::to_string(gen.below(10));
        switch (gen.below(3)) {
        case 0:
          events.push_back(trade_at(ts, px));
          break;
        case 1:
          events.push_back(quote_at(ts, px, px));
          break;
        default:
          events.push_back(batch_end(ts));
          break;
        }
      }
      const auto run = [&](Log& log, std::vector<md::Output>& outputs) {
        st::StaticStrategySet<Recorder, Recorder, Recorder> set{
            Recorder{"bars", &log}, Recorder{"feature", &log}, Recorder{"quotes:conflated", &log}};
        jarvis::engine::Engine engine{small_config(), set};
        std::uint64_t seq = 0;
        REQUIRE(drive(engine, events, seq) == Status::Ok);
        outputs.assign(engine.outputs().begin(), engine.outputs().end());
      };
      Log a;
      Log b;
      std::vector<md::Output> oa;
      std::vector<md::Output> ob;
      run(a, oa);
      run(b, ob);
      CHECK(a == b);
      REQUIRE(oa.size() == ob.size());
    });
  }

  TEST_CASE("random reason sequences follow the table and never leave a terminal state") {
    jarvis::testkit::for_all([](Gen& gen) {
      e::Lifecycle machine;
      jarvis::model::NodeLifecycle event;
      for (int step = 0; step < 64; ++step) {
        const LifecycleReason r = gen.pick(std::span<const LifecycleReason>{kReasons});
        const NodeState before = machine.state();
        NodeState expected = before;
        const Status table = e::next_state(before, r, expected);
        const Status applied = machine.apply(r, UnixNanos{static_cast<std::uint64_t>(step)}, event);
        CHECK(applied == table);
        CHECK(machine.state() == (jarvis::core::ok(applied) ? expected : before));
        if (e::is_terminal(before)) {
          CHECK(machine.state() == before);
        }
      }
    });
  }
}

namespace {

// Counts callbacks without allocating.
// Test strategies write through pointers to state owned by the test; real strategies mutate
// their own members, so the callbacks stay non-const as they would be there.
// NOLINTBEGIN(readability-make-member-function-const)
struct Counter {
  std::uint64_t* trades = nullptr;
  md::FeatureId feature = 0;
  Status on_start(st::Context& ctx) {
    const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
    Status s = ctx.subscribe_trades(btc);
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_quotes(btc, dt::Cadence::conflated());
    }
    if (jarvis::core::ok(s)) {
      s = ctx.feature(dt::FeatureSpec{dt::FeatureKind::Vwap, btc, 32}, dt::Cadence::sampled_ms(1),
                      feature);
    }
    md::BarType type;
    if (jarvis::core::ok(s) &&
        jarvis::core::ok(md::BarType::parse("BTCUSDT-PERP.BINANCE-5-TICK-LAST-INTERNAL", type))) {
      s = ctx.subscribe_bars(type);
    }
    return s;
  }
  Status on_trade(st::Context& /*ctx*/, const md::TradeTick& /*t*/) {
    ++*trades;
    return Status::Ok;
  }
};

// Every other delivery path: L2 book deltas and conflated book views, time bars closed by kernel
// timers, OnBatch trade buffers, a feature delivered on every quote, and records as outputs.
struct Full {
  std::uint64_t* calls = nullptr;
  static Status on_start(st::Context& ctx) {
    const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
    Status s = ctx.subscribe_book_deltas(btc);
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_book(btc, dt::Cadence::conflated(), md::BookType::L2_MBP);
    }
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_trades(btc, dt::Cadence::on_batch());
    }
    md::FeatureId id = 0;
    if (jarvis::core::ok(s)) {
      s = ctx.feature(dt::FeatureSpec{dt::FeatureKind::Imbalance, btc, 0}, dt::Cadence::every(),
                      id);
    }
    md::BarType type;
    if (jarvis::core::ok(s) &&
        jarvis::core::ok(md::BarType::parse("BTCUSDT-PERP.BINANCE-1-SECOND-LAST-INTERNAL", type))) {
      s = ctx.subscribe_bars(type);
    }
    return s;
  }
  void on_book_deltas(st::Context& /*ctx*/, const md::OrderBookDeltas& d) {
    *calls += d.deltas.size();
  }
  static void on_book(st::Context& ctx, const dt::BookView& book) {
    dt::BookLevel best;
    if (book.best_bid(best)) {
      md::Decimal v;
      static_cast<void>(md::Decimal::from_raw(best.price.raw(), best.price.precision(), v));
      static_cast<void>(ctx.record("bid", v));
    }
  }
  void on_trade_batch(st::Context& /*ctx*/, const st::TradeBatch& b) { *calls += b.trades.size(); }
  void on_feature(st::Context& /*ctx*/, md::FeatureId /*id*/, md::Decimal /*v*/, UnixNanos /*ts*/) {
    ++*calls;
  }
  void on_bar(st::Context& /*ctx*/, const md::Bar& /*bar*/) { ++*calls; }
};
// NOLINTEND(readability-make-member-function-const)

md::OrderBookDelta book_delta(std::uint64_t ts, md::BookAction action, md::OrderSide side,
                              std::string_view px, std::string_view size, std::uint8_t flags) {
  md::OrderBookDelta d;
  d.instrument_id = iid("BTCUSDT-PERP.BINANCE");
  d.action = action;
  d.order.side = side;
  d.order.price = price(px);
  d.order.size = quantity(size);
  d.flags = flags;
  d.sequence = ts;
  d.ts_event = UnixNanos{ts};
  d.ts_init = UnixNanos{ts};
  return d;
}

} // namespace

TEST_SUITE("zero-alloc") {
  TEST_CASE("every delivery path runs without allocating once warmed up") {
    std::uint64_t calls = 0;
    st::StaticStrategySet<Full> set{Full{&calls}};
    jarvis::engine::Engine engine{small_config(), set};
    // Storage for the deltas the OrderBookDeltas events point into.
    std::vector<std::array<md::OrderBookDelta, 2>> storage;
    storage.reserve(3000);
    std::vector<md::Event> events{running(1)};
    std::uint64_t ts = 1'000'000;
    for (std::uint64_t i = 0; i < 3000; ++i) {
      ts += 50'000'000; // 20 events per second: time bars close every 20 events
      const std::string bid = std::to_string(100 + i % 3) + "." + std::to_string(i % 10);
      const std::string ask = std::to_string(103 + i % 3) + "." + std::to_string(i % 10);
      switch (i % 5) {
      case 0: {
        storage.push_back(
            {book_delta(ts, md::BookAction::Update, md::OrderSide::Buy, bid, "1.000", 0),
             book_delta(ts, md::BookAction::Update, md::OrderSide::Sell, ask, "2.000", 128)});
        md::OrderBookDeltas deltas;
        REQUIRE(md::OrderBookDeltas::create(storage.back(), deltas) == Status::Ok);
        events.push_back(md::Event{deltas});
        break;
      }
      case 1:
      case 2:
        events.push_back(trade_at(ts, bid));
        break;
      case 3:
        events.push_back(quote_at(ts, bid, ask));
        break;
      default:
        events.push_back(batch_end(ts));
        break;
      }
    }
    std::uint64_t seq = 0;
    const std::vector<md::Event> warmup(events.begin(), events.begin() + 200);
    REQUIRE(drive(engine, warmup, seq) == Status::Ok);
    engine.clear_outputs();
    const std::vector<md::Event> rest(events.begin() + 200, events.end());
    const AllocationScope scope;
    for (const md::Event& e : rest) {
      const UnixNanos at = md::ts_init_of(e);
      jarvis::core::FiredTimer due;
      while (engine.next_timer(due) && due.deadline <= at) {
        static_cast<void>(
            engine.step(EventKey{due.deadline, 9, ++seq},
                        md::Event{md::TimerFired{due.key, due.deadline, due.deadline}}));
        engine.clear_outputs();
      }
      static_cast<void>(engine.step(EventKey{at, 0, ++seq}, e));
      engine.clear_outputs();
    }
    CHECK(scope.allocations() == 0);
    CHECK(engine.failures().empty());
    CHECK(calls > 3000);
  }

  TEST_CASE("steady-state steps do not allocate") {
    std::uint64_t trades = 0;
    st::StaticStrategySet<Counter> set{Counter{&trades}};
    jarvis::engine::Engine engine{small_config(), set};
    std::vector<md::Event> events{running(1)};
    for (std::uint64_t i = 0; i < 2000; ++i) {
      if (i % 3 == 0) {
        events.push_back(trade_at(10 + i, "100.5"));
      } else if (i % 3 == 1) {
        events.push_back(quote_at(10 + i, "100.4", "100.6"));
      } else {
        events.push_back(batch_end(10 + i));
      }
    }
    std::uint64_t seq = 0;
    const std::vector<md::Event> warmup(events.begin(), events.begin() + 20);
    REQUIRE(drive(engine, warmup, seq) == Status::Ok);
    engine.clear_outputs();
    const std::vector<md::Event> rest(events.begin() + 20, events.end());
    const AllocationScope scope;
    for (const md::Event& e : rest) {
      const UnixNanos ts = md::ts_init_of(e);
      static_cast<void>(engine.step(EventKey{ts, 0, ++seq}, e));
      engine.clear_outputs();
    }
    CHECK(scope.allocations() == 0);
    CHECK(trades > 600);
  }
}

// ---- orders -----------------------------------------------------------------------------------
// Test strategies write through pointers to state owned by the test (see Recorder above).
// NOLINTBEGIN(readability-make-member-function-const,readability-convert-member-functions-to-static)

namespace {

namespace ex = jarvis::execution;

template <typename Id> Id make_id(const std::string& text) {
  Id out;
  REQUIRE(Id::from(text, out) == Status::Ok);
  return out;
}

md::Event perpetual_definition(std::uint64_t ts) {
  md::CryptoPerpetual p;
  md::InstrumentCommon& c = p.common;
  c.id = iid("BTCUSDT-PERP.BINANCE");
  c.raw_symbol = make_id<md::Symbol>("BTCUSDT");
  md::Currency btc;
  md::Currency usdt;
  REQUIRE(md::Currency::builtin("BTC", btc) == Status::Ok);
  REQUIRE(md::Currency::builtin("USDT", usdt) == Status::Ok);
  c.base_currency = btc;
  c.quote_currency = usdt;
  c.settlement_currency = usdt;
  c.price_increment = price("0.1");
  c.price_precision = 1;
  c.size_increment = quantity("0.001");
  c.size_precision = 3;
  c.multiplier = quantity("1");
  REQUIRE(md::Decimal::parse("0.05", c.margin_init) == Status::Ok);
  REQUIRE(md::Decimal::parse("0.025", c.margin_maint) == Status::Ok);
  c.ts_event = UnixNanos{ts};
  c.ts_init = UnixNanos{ts};
  return md::Event{p};
}

template <typename E> E venue_event(std::uint64_t ts, const md::ClientOrderId& id) {
  E e;
  e.header.instrument_id = iid("BTCUSDT-PERP.BINANCE");
  e.header.client_order_id = id;
  e.header.ts_event = UnixNanos{ts};
  e.header.ts_init = UnixNanos{ts};
  return e;
}
md::Event accepted(std::uint64_t ts, const md::ClientOrderId& id, const std::string& venue_id) {
  auto e = venue_event<md::OrderAccepted>(ts, id);
  e.venue_order_id = make_id<md::VenueOrderId>(venue_id);
  return md::Event{e};
}
md::Event filled(std::uint64_t ts, const md::ClientOrderId& id, const std::string& trade,
                 std::string_view qty, std::string_view px) {
  auto e = venue_event<md::OrderFilled>(ts, id);
  e.trade_id = make_id<md::TradeId>(trade);
  e.last_qty = quantity(qty);
  e.last_px = price(px);
  return md::Event{e};
}
md::Event updated(std::uint64_t ts, const md::ClientOrderId& id, std::string_view qty,
                  std::string_view px) {
  auto e = venue_event<md::OrderUpdated>(ts, id);
  e.quantity = quantity(qty);
  e.price = price(px);
  return md::Event{e};
}
md::Event canceled(std::uint64_t ts, const md::ClientOrderId& id) {
  return md::Event{venue_event<md::OrderCanceled>(ts, id)};
}

std::string order_line(const md::OrderEvent& e) {
  ex::OrderEventKind kind{};
  REQUIRE(ex::kind_of(e, kind));
  std::string line{ex::to_string(kind)};
  if (const auto* d = std::get_if<md::OrderDenied>(&e)) {
    line += " ";
    line += d->reason.view();
  }
  return line;
}

using Script = std::function<Status(st::Context&, int)>;

// Runs `script` in on_start (step 0) and on every trade (steps 1, 2, ...); logs order events.
struct Trader {
  Script* script = nullptr;
  std::vector<std::string>* log = nullptr;
  int trades = 0;

  Status on_start(st::Context& ctx) {
    REQUIRE(ctx.subscribe_trades(iid("BTCUSDT-PERP.BINANCE")) == Status::Ok);
    return (*script)(ctx, 0);
  }
  Status on_trade(st::Context& ctx, const md::TradeTick& /*t*/) { return (*script)(ctx, ++trades); }
  Status on_order_event(st::Context& /*ctx*/, const md::OrderEvent& e) {
    log->push_back(order_line(e));
    return Status::Ok;
  }
};

template <typename T> std::size_t count_outputs(std::span<const md::Output> outputs) {
  std::size_t n = 0;
  for (const md::Output& o : outputs) {
    n += std::holds_alternative<T>(o) ? 1U : 0U;
  }
  return n;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("orders go through submit, venue answers, modify and cancel") {
    const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
    std::vector<std::string> log;
    std::vector<md::ClientOrderId> ids(3);
    st::OrderView view;
    Script script = [&](st::Context& ctx, int step) -> Status {
      switch (step) {
      case 0:
        REQUIRE(ctx.submit(ctx.limit(btc, md::OrderSide::Buy, quantity("1.000"), price("100.0")),
                           ids[0]) == Status::Ok);
        REQUIRE(ctx.submit(ctx.limit(btc, md::OrderSide::Buy, quantity("1.000"), price("100.05")),
                           ids[1]) == Status::Ok);
        {
          st::OrderIntent market = ctx.market(btc, md::OrderSide::Sell, quantity("0.500"));
          market.price = price("99.0");
          REQUIRE(ctx.submit(market, ids[2]) == Status::Ok);
        }
        break;
      case 1:
        CHECK(ctx.modify(ids[0], std::nullopt, price("99.9")) == Status::Ok);
        CHECK(ctx.modify(ids[1], std::nullopt, price("99.9")) == Status::InvalidState); // denied
        break;
      case 2:
        CHECK(ctx.cancel(ids[0]) == Status::Ok);
        CHECK(ctx.cancel(ids[0]) == Status::InvalidState); // one cancel in flight is enough
        CHECK(ctx.modify(ids[0], quantity("2.000"), std::nullopt) == Status::InvalidState);
        break;
      default:
        REQUIRE(ctx.order(ids[0], view));
        break;
      }
      return Status::Ok;
    };
    st::StaticStrategySet<Trader> set{Trader{&script, &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {perpetual_definition(1), running(2)}, seq) == Status::Ok);
    CHECK(log == std::vector<std::string>{"SUBMITTED", "DENIED PRICE_INVALID_PRECISION",
                                          "DENIED PRICE_UNEXPECTED"});
    CHECK(count_outputs<md::SubmitOrder>(engine.outputs()) == 1);
    CHECK(count_outputs<md::OrderDenied>(engine.outputs()) == 2);
    CHECK(ids[0].view() == "jarvis-000001-00000001");

    log.clear();
    REQUIRE(
        drive(engine,
              {accepted(3, ids[0], "v1"), trade_at(4, "100.0"), updated(5, ids[0], "1.000", "99.9"),
               trade_at(6, "99.9"), filled(7, ids[0], "t1", "0.400", "99.9"),
               filled(8, ids[0], "t1", "0.400", "99.9"), trade_at(9, "99.9")},
              seq) == Status::Ok);
    CHECK(log == std::vector<std::string>{"ACCEPTED", "PENDING_UPDATE", "UPDATED", "PENDING_CANCEL",
                                          "FILLED"});
    CHECK(view.status == md::OrderStatus::PendingCancel); // the fill keeps the cancel pending
    CHECK(view.filled == quantity("0.400"));
    CHECK(view.leaves == quantity("0.600"));
    CHECK(view.venue_order_id == make_id<md::VenueOrderId>("v1"));
    CHECK(view.avg_px == price("99.9"));
    const std::span<const md::Output> out = engine.outputs();
    REQUIRE(count_outputs<md::ModifyOrder>(out) == 1);
    REQUIRE(count_outputs<md::CancelOrder>(out) == 1);
    for (const md::Output& o : out) {
      if (const auto* m = std::get_if<md::ModifyOrder>(&o)) {
        CHECK(m->price == price("99.9"));
        CHECK(m->quantity == quantity("1.000"));
        CHECK(m->venue_order_id == make_id<md::VenueOrderId>("v1"));
      }
    }

    log.clear();
    const md::ClientOrderId stranger = make_id<md::ClientOrderId>("other-1");
    REQUIRE(drive(engine, {canceled(10, ids[0]), canceled(11, ids[0]), canceled(12, stranger)},
                  seq) == Status::Ok);
    CHECK(log == std::vector<std::string>{"CANCELED"});
    const st::TradingStats& stats = engine.kernel().trading.stats;
    CHECK(stats.submitted == 1);
    CHECK(stats.denied == 2);
    CHECK(stats.duplicate_fills == 1);
    CHECK(stats.refused_order_events == 1); // CANCELED twice
    CHECK(stats.unknown_order_events == 1);
    CHECK(engine.failures().empty());
  }

  TEST_CASE("orders for an instrument without a definition are denied") {
    const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
    std::vector<std::string> log;
    md::ClientOrderId id;
    Script script = [&](st::Context& ctx, int step) -> Status {
      if (step == 0) {
        md::Instrument def;
        CHECK_FALSE(ctx.instrument(btc, def));
        REQUIRE(ctx.submit(ctx.market(btc, md::OrderSide::Buy, quantity("0.001")), id) ==
                Status::Ok);
      }
      return Status::Ok;
    };
    st::StaticStrategySet<Trader> set{Trader{&script, &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {running(1)}, seq) == Status::Ok);
    CHECK(log == std::vector<std::string>{"DENIED INSTRUMENT_UNKNOWN"});
    REQUIRE(engine.outputs().size() == 1);
    const auto& denied = std::get<md::OrderDenied>(engine.outputs()[0]);
    CHECK(denied.header.client_order_id == id);
    CHECK(denied.header.strategy_id.view() == "strategy-001");
    CHECK(denied.header.trader_id.view() == "JARVIS-001");
  }

  TEST_CASE("an instrument definition sets the book tick and is visible to strategies") {
    std::vector<std::string> log;
    Script script = [&](st::Context& ctx, int /*step*/) -> Status {
      md::Instrument def;
      REQUIRE(ctx.instrument(iid("BTCUSDT-PERP.BINANCE"), def));
      CHECK(md::common(def).price_increment == price("0.1"));
      return Status::Ok;
    };
    st::StaticStrategySet<Trader> set{Trader{&script, &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {perpetual_definition(1), running(2)}, seq) == Status::Ok);
    md::InstrumentSlot slot;
    REQUIRE(engine.kernel().instruments.find(iid("BTCUSDT-PERP.BINANCE"), slot) == Status::Ok);
    CHECK(engine.kernel().ticks[slot.value] == price("0.1"));

    md::Event bad = perpetual_definition(3);
    std::get<md::CryptoPerpetual>(bad).common.price_precision = 4; // disagrees with the increment
    CHECK(engine.step(EventKey{UnixNanos{3}, 0, ++seq}, bad) == Status::InvalidArgument);
  }

  TEST_CASE("commands issued from on_order_event run in the same step, up to the queue") {
    // Resubmits after every SUBMITTED: stops only when the event queue is full.
    struct Chain {
      std::uint32_t* submitted = nullptr;
      Status last = Status::Ok;
      Status on_start(st::Context& ctx) { return submit(ctx); }
      Status on_order_event(st::Context& ctx, const md::OrderEvent& e) {
        if (std::holds_alternative<md::OrderSubmitted>(e)) {
          ++*submitted;
          last = submit(ctx);
        }
        return Status::Ok;
      }
      static Status submit(st::Context& ctx) {
        md::ClientOrderId id;
        return ctx.submit(ctx.limit(iid("BTCUSDT-PERP.BINANCE"), md::OrderSide::Buy,
                                    quantity("0.001"), price("100.0")),
                          id);
      }
    };
    std::uint32_t submitted = 0;
    st::StaticStrategySet<Chain> set{Chain{&submitted}};
    st::KernelConfig config = small_config();
    config.trading.order_events = 8;
    config.trading.orders = 64;
    jarvis::engine::Engine engine{config, set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {perpetual_definition(1), running(2)}, seq) == Status::Ok);
    CHECK(submitted == 8);
    CHECK(set.get<0>().last == Status::CapacityExceeded);
    CHECK(count_outputs<md::SubmitOrder>(engine.outputs()) == 8);
    CHECK(engine.kernel().trading.events.empty());
  }

  TEST_CASE("a halted strategy's open orders are canceled") {
    const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
    std::vector<std::string> log;
    md::ClientOrderId id;
    Script script = [&](st::Context& ctx, int step) -> Status {
      if (step == 0) {
        REQUIRE(ctx.submit(ctx.limit(btc, md::OrderSide::Sell, quantity("0.010"), price("101.0")),
                           id) == Status::Ok);
        return Status::Ok;
      }
      return Status::InvalidState; // the first trade fails the strategy
    };
    st::StaticStrategySet<Trader> set{Trader{&script, &log}};
    jarvis::engine::Engine engine{small_config(), set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {perpetual_definition(1), running(2)}, seq) == Status::Ok);
    REQUIRE(drive(engine, {accepted(3, id, "v7"), trade_at(4, "100.0")}, seq) == Status::Ok);
    REQUIRE(engine.failures().size() == 1);
    engine.clear_outputs();
    const md::Event error{md::StrategyError{0, md::StrategyErrorKind::Exception, 1, UnixNanos{4}}};
    REQUIRE(engine.step(EventKey{UnixNanos{4}, 0, ++seq}, error) == Status::Ok);
    REQUIRE(engine.outputs().size() == 1);
    const auto& cancel = std::get<md::CancelOrder>(engine.outputs()[0]);
    CHECK(cancel.client_order_id == id);
    CHECK(log == std::vector<std::string>{"SUBMITTED", "ACCEPTED"}); // nothing after the halt
  }

  TEST_CASE("event ids are derived from the seed and the input, never repeated") {
    const auto run = [](std::uint64_t seed) {
      const md::InstrumentId btc = iid("BTCUSDT-PERP.BINANCE");
      std::vector<std::string> log;
      std::vector<md::Uuid4> event_ids;
      Script script = [&](st::Context& ctx, int /*step*/) -> Status {
        for (int i = 0; i < 3; ++i) {
          md::ClientOrderId id;
          st::OrderIntent bad = ctx.limit(btc, md::OrderSide::Buy, quantity("1.0"), price("1.0"));
          REQUIRE(ctx.submit(bad, id) == Status::Ok); // denied: quantity precision
        }
        return Status::Ok;
      };
      st::StaticStrategySet<Trader> set{Trader{&script, &log}};
      st::KernelConfig config = small_config();
      config.seed = seed;
      jarvis::engine::Engine engine{config, set};
      std::uint64_t seq = 0;
      REQUIRE(drive(engine, {perpetual_definition(1), running(2), trade_at(3, "1.0")}, seq) ==
              Status::Ok);
      for (const md::Output& o : engine.outputs()) {
        event_ids.push_back(std::get<md::OrderDenied>(o).header.event_id);
      }
      return event_ids;
    };
    const std::vector<md::Uuid4> a = run(7);
    CHECK(a.size() == 6);
    CHECK(a == run(7));
    CHECK(a != run(8));
    CHECK(std::set<md::Uuid4>(a.begin(), a.end()).size() == a.size());
  }
}

TEST_SUITE("zero-alloc") {
  TEST_CASE("submit, venue answers and cancel run without allocating") {
    // Quotes a bid on every trade and cancels the previous one.
    struct Quoter {
      md::ClientOrderId* last = nullptr;
      std::uint64_t* events = nullptr;
      Status on_start(st::Context& ctx) {
        return ctx.subscribe_trades(iid("BTCUSDT-PERP.BINANCE"));
      }
      Status on_trade(st::Context& ctx, const md::TradeTick& t) {
        if (!last->empty()) {
          static_cast<void>(ctx.cancel(*last));
        }
        return ctx.submit(
            ctx.limit(t.instrument_id, md::OrderSide::Buy, quantity("0.001"), t.price), *last);
      }
      Status on_order_event(st::Context& /*ctx*/, const md::OrderEvent& /*e*/) {
        ++*events;
        return Status::Ok;
      }
    };
    md::ClientOrderId last;
    std::uint64_t events = 0;
    st::StaticStrategySet<Quoter> set{Quoter{&last, &events}};
    st::KernelConfig config = small_config();
    config.trading.orders = 32; // eviction of closed orders is part of the steady state
    jarvis::engine::Engine engine{config, set};
    std::uint64_t seq = 0;
    REQUIRE(drive(engine, {perpetual_definition(1), running(2)}, seq) == Status::Ok);
    const auto cycle = [&](std::uint64_t ts) {
      static_cast<void>(engine.step(EventKey{UnixNanos{ts}, 0, ++seq}, trade_at(ts, "100.0")));
      const md::ClientOrderId id = last;
      static_cast<void>(
          engine.step(EventKey{UnixNanos{ts + 1}, 0, ++seq}, accepted(ts + 1, id, "v")));
      static_cast<void>(
          engine.step(EventKey{UnixNanos{ts + 2}, 0, ++seq},
                      filled(ts + 2, id, "t" + std::to_string(ts), "0.001", "100.0")));
      engine.clear_outputs();
    };
    for (std::uint64_t i = 0; i < 100; ++i) {
      cycle(10 + i * 10);
    }
    // Inputs are built before the scope (the helpers allocate strings); inside it only the
    // ClientOrderId of the venue answers is filled in.
    std::vector<md::Event> trades;
    std::vector<md::Event> acks;
    std::vector<md::Event> fills;
    for (std::uint64_t i = 0; i < 500; ++i) {
      const std::uint64_t ts = 10'000 + i * 10;
      trades.push_back(trade_at(ts, "100.0"));
      acks.push_back(accepted(ts + 1, last, "v"));
      fills.push_back(filled(ts + 2, last, "u" + std::to_string(i), "0.001", "100.0"));
    }
    const AllocationScope scope;
    for (std::size_t i = 0; i < trades.size(); ++i) {
      const std::uint64_t ts = 10'000 + i * 10;
      static_cast<void>(engine.step(EventKey{UnixNanos{ts}, 0, ++seq}, trades[i]));
      std::get<md::OrderAccepted>(acks[i]).header.client_order_id = last;
      static_cast<void>(engine.step(EventKey{UnixNanos{ts + 1}, 0, ++seq}, acks[i]));
      std::get<md::OrderFilled>(fills[i]).header.client_order_id = last;
      static_cast<void>(engine.step(EventKey{UnixNanos{ts + 2}, 0, ++seq}, fills[i]));
      engine.clear_outputs();
    }
    CHECK(scope.allocations() == 0);
    CHECK(engine.failures().empty());
    CHECK(events > 1500);
    CHECK(engine.kernel().trading.stats.refused_order_events == 0);
  }
}
// NOLINTEND(readability-make-member-function-const,readability-convert-member-functions-to-static)
