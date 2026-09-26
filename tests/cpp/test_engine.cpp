#include <array>
#include <cstddef>
#include <cstdint>
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
#include "jarvis/model/event.hpp"
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
// NOLINTEND(readability-make-member-function-const)

} // namespace

TEST_SUITE("zero-alloc") {
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
    std::size_t index = 0;
    const AllocationScope scope;
    for (const md::Event& e : rest) {
      const UnixNanos ts = md::ts_init_of(e);
      static_cast<void>(engine.step(EventKey{ts, 0, ++seq}, e));
      engine.clear_outputs();
      ++index;
    }
    CHECK(scope.allocations() == 0);
    CHECK(trades > 600);
  }
}
