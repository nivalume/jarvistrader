#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/backtest/driver.hpp"
#include "jarvis/backtest/merge_source.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/engine/sync_gate.hpp"
#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/testkit/alloc.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace bt = jarvis::backtest;
namespace st = jarvis::strategy;
namespace md = jarvis::model;
namespace wire = jarvis::model::wire;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::testkit::Gen;

md::InstrumentId btc() {
  md::InstrumentId id;
  REQUIRE(md::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
}

md::Event trade(std::uint64_t ts, std::int64_t price_tenths) {
  md::TradeTick t;
  t.instrument_id = btc();
  REQUIRE(md::Price::from_raw(price_tenths * 100'000'000, 1, t.price) == Status::Ok);
  REQUIRE(md::Quantity::from_raw(1'000'000'000, 3, t.size) == Status::Ok);
  t.aggressor_side = md::AggressorSide::Buy;
  REQUIRE(md::TradeId::from(std::to_string(ts), t.trade_id) == Status::Ok);
  t.ts_event = UnixNanos{ts};
  t.ts_init = UnixNanos{ts};
  return md::Event{t};
}

md::Event quote(std::uint64_t ts, std::int64_t bid_tenths) {
  md::QuoteTick q;
  q.instrument_id = btc();
  REQUIRE(md::Price::from_raw(bid_tenths * 100'000'000, 1, q.bid_price) == Status::Ok);
  REQUIRE(md::Price::from_raw((bid_tenths + 1) * 100'000'000, 1, q.ask_price) == Status::Ok);
  REQUIRE(md::Quantity::from_raw(1'000'000'000, 3, q.bid_size) == Status::Ok);
  REQUIRE(md::Quantity::from_raw(2'000'000'000, 3, q.ask_size) == Status::Ok);
  q.ts_event = UnixNanos{ts};
  q.ts_init = UnixNanos{ts};
  return md::Event{q};
}

struct Keyed {
  EventKey key;
  md::Event event;
};

// A source over a vector of events, as a decoded data log would yield them.
class VectorSource {
public:
  explicit VectorSource(std::vector<Keyed> items) : items_{std::move(items)} {}
  Status next(EventKey& key, md::Event& event) {
    if (index_ >= items_.size()) {
      return Status::EndOfStream;
    }
    key = items_[index_].key;
    event = items_[index_].event;
    ++index_;
    return Status::Ok;
  }

private:
  std::vector<Keyed> items_;
  std::size_t index_ = 0;
};

struct KeyedOutput {
  EventKey key;
  md::Output output;
};

class MemoryRecorder {
public:
  Status record(const EventKey& key, const md::Event& event) {
    inputs.push_back(Keyed{key, event});
    return Status::Ok;
  }
  Status emit(const EventKey& key, const md::Output& output) {
    outputs.push_back(KeyedOutput{key, output});
    return Status::Ok;
  }
  std::vector<Keyed> inputs;
  std::vector<KeyedOutput> outputs;
};

// Counts without storing: the recorder for the zero-allocation gate.
struct CountingRecorder {
  std::uint64_t inputs = 0;
  std::uint64_t outputs = 0;
  Status record(const EventKey& /*key*/, const md::Event& /*event*/) {
    ++inputs;
    return Status::Ok;
  }
  Status emit(const EventKey& /*key*/, const md::Output& /*output*/) {
    ++outputs;
    return Status::Ok;
  }
};

std::string describe(const Keyed& k) {
  return std::to_string(k.key.seq) + " " + std::string{wire::kind_name(wire::kind_of(k.event))} +
         "@" + std::to_string(k.key.ts.value());
}

std::vector<std::string> describe(const std::vector<Keyed>& inputs) {
  std::vector<std::string> out;
  out.reserve(inputs.size());
  for (const Keyed& k : inputs) {
    out.push_back(describe(k));
  }
  return out;
}

std::vector<std::byte> encode(const KeyedOutput& o) {
  std::vector<std::byte> buffer(wire::kRecordHeaderSize + wire::kMaxPayload + 4);
  std::size_t written = 0;
  REQUIRE(wire::encode_output_record(o.key, o.output, buffer, written) == Status::Ok);
  buffer.resize(written);
  return buffer;
}

st::KernelConfig small_config() {
  st::KernelConfig c;
  c.instruments = 4;
  c.strategies = 4;
  c.timers = 16;
  c.batch = 8;
  c.features = 4;
  c.bar_types = 4;
  c.buffers = 16;
  c.outputs = 64;
  c.book_window_levels = 256;
  c.book_overflow_levels = 64;
  return c;
}

// Test strategies write through a pointer to a log the test owns; real strategies mutate their
// own members, so the callbacks stay non-const as they would be there.
// NOLINTBEGIN(readability-make-member-function-const)
struct Echo {
  std::vector<std::string>* log = nullptr;
  std::uint64_t timer_at = 0;   // on_start arms timer 1 at this deadline (0: none)
  std::uint64_t period = 0;     // and repeats it with this period
  std::uint64_t fail_trade = 0; // on_trade fails on this trade (1-based; 0: never)
  std::uint64_t trades = 0;

  Status on_start(st::Context& ctx) {
    log->push_back("start@" + std::to_string(ctx.now().value()));
    Status s = ctx.subscribe_trades(btc());
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_quotes(btc(), jarvis::data::Cadence::conflated());
    }
    if (jarvis::core::ok(s) && timer_at != 0) {
      s = ctx.set_timer(1, UnixNanos{timer_at}, jarvis::core::DurationNanos{period});
    }
    return s;
  }
  Status on_trade(st::Context& ctx, const md::TradeTick& t) {
    ++trades;
    log->push_back("trade@" + std::to_string(t.ts_init.value()));
    md::Decimal px;
    static_cast<void>(md::Decimal::from_raw(t.price.raw(), t.price.precision(), px));
    static_cast<void>(ctx.record("px", px));
    return trades == fail_trade ? Status::InvalidArgument : Status::Ok;
  }
  void on_quote(st::Context& /*ctx*/, const md::QuoteTick& q) {
    log->push_back("quote@" + std::to_string(q.ts_init.value()));
  }
  void on_timer(st::Context& ctx, jarvis::core::TimerKey key, UnixNanos deadline) {
    log->push_back("timer" + std::to_string(key.id) + "@" + std::to_string(deadline.value()) +
                   " seq" + std::to_string(ctx.seq()));
  }
  void on_error(st::Context& /*ctx*/, const md::StrategyError& /*e*/) { log->push_back("error"); }
  void on_stop(st::Context& ctx) { log->push_back("stop@" + std::to_string(ctx.now().value())); }
};

struct Counter {
  std::uint64_t* trades = nullptr;
  static Status on_start(st::Context& ctx) { return ctx.subscribe_trades(btc()); }
  void on_trade(st::Context& /*ctx*/, const md::TradeTick& /*t*/) { ++*trades; }
};
// NOLINTEND(readability-make-member-function-const)

struct Run {
  std::vector<std::string> calls;
  MemoryRecorder recorder;
  bt::RunSummary summary;
};

Run run_echo(std::vector<Keyed> data, Echo echo, bt::DriverOptions options = {},
             st::ErrorPolicy policy = st::ErrorPolicy::HaltStrategy) {
  Run r;
  echo.log = &r.calls;
  st::StaticStrategySet<Echo> set{echo};
  jarvis::engine::Engine engine{small_config(), set, policy};
  VectorSource source{std::move(data)};
  bt::Driver driver{engine, source, r.recorder, options};
  REQUIRE(driver.run(r.summary) == Status::Ok);
  return r;
}

EventKey key(std::uint64_t ts, std::uint16_t source, std::uint64_t seq) {
  return EventKey{UnixNanos{ts}, source, seq};
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the merge serves sources in key order, ties by the order they were added") {
    VectorSource a{{{key(10, 1, 1), trade(10, 1)}, {key(30, 1, 2), trade(30, 3)}}};
    VectorSource b{{{key(10, 1, 1), quote(10, 1)}, {key(20, 2, 1), quote(20, 2)}}};
    VectorSource empty{{}};
    bt::MergeSource<VectorSource> merge{3};
    REQUIRE(merge.add(a) == Status::Ok);
    REQUIRE(merge.add(empty) == Status::Ok);
    REQUIRE(merge.add(b) == Status::Ok);
    std::vector<std::string> seen;
    EventKey k;
    md::Event e;
    while (merge.next(k, e) == Status::Ok) {
      seen.push_back(std::string{wire::kind_name(wire::kind_of(e))} + "@" +
                     std::to_string(k.ts.value()));
    }
    CHECK(seen ==
          std::vector<std::string>{"TradeTick@10", "QuoteTick@10", "QuoteTick@20", "TradeTick@30"});
    CHECK(merge.next(k, e) == Status::EndOfStream);
    CHECK(merge.add(a) == Status::InvalidState);
  }

  TEST_CASE("a source out of key order is reported") {
    VectorSource a{{{key(20, 1, 1), trade(20, 1)}, {key(10, 1, 2), trade(10, 1)}}};
    bt::MergeSource<VectorSource> merge{1};
    REQUIRE(merge.add(a) == Status::Ok);
    EventKey k;
    md::Event e;
    CHECK(merge.next(k, e) == Status::Ok);
    CHECK(merge.next(k, e) == Status::InvalidArgument);
  }

  TEST_CASE("the driver records lifecycle, timers, batches and outputs around the data") {
    const Run r = run_echo({{key(10, 1, 1), trade(10, 1000)},
                            {key(10, 2, 1), quote(10, 999)},
                            {key(20, 1, 2), trade(20, 1001)}},
                           Echo{nullptr, 15});
    CHECK(describe(r.recorder.inputs) ==
          std::vector<std::string>{"1 NodeLifecycle@10", "2 NodeLifecycle@10", "3 NodeLifecycle@10",
                                   "4 NodeLifecycle@10", "5 TradeTick@10", "6 QuoteTick@10",
                                   "7 BatchEnd@10", "8 TimerFired@15", "9 BatchEnd@15",
                                   "10 TradeTick@20", "11 NodeLifecycle@20", "12 NodeLifecycle@20",
                                   "13 BatchEnd@20"});
    CHECK(r.calls == std::vector<std::string>{"start@10", "trade@10", "quote@10", "timer1@15 seq8",
                                              "trade@20", "stop@20"});
    // Data keeps its source id; synthesised inputs use the kernel's.
    CHECK(r.recorder.inputs[4].key.source_id == 1);
    CHECK(r.recorder.inputs[5].key.source_id == 2);
    CHECK(r.recorder.inputs[7].key.source_id == bt::kKernelSource);
    // Each StrategyRecord output is keyed by the trade that caused it.
    REQUIRE(r.recorder.outputs.size() == 2);
    CHECK(r.recorder.outputs[0].key == key(10, 0, 5));
    CHECK(r.recorder.outputs[1].key == key(20, 0, 10));
    CHECK(r.summary.data_events == 3);
    CHECK(r.summary.inputs == 13);
    CHECK(r.summary.outputs == 2);
    CHECK(r.summary.batches == 3);
    CHECK(r.summary.timers == 1);
    CHECK(r.summary.state == md::NodeState::Stopped);
    CHECK_FALSE(r.summary.halted);
    const auto& last = std::get<md::NodeLifecycle>(r.recorder.inputs[11].event);
    CHECK(last.to == md::NodeState::Stopped);
    CHECK(std::get<md::NodeLifecycle>(r.recorder.inputs[10].event).reason ==
          md::LifecycleReason::EndOfData);
  }

  TEST_CASE("a timer due at a data event's ts fires before it, in the same batch") {
    const Run r = run_echo({{key(10, 1, 1), trade(10, 1000)}, {key(15, 1, 2), trade(15, 1000)}},
                           Echo{nullptr, 15});
    CHECK(describe(r.recorder.inputs)[5] == "6 BatchEnd@10");
    CHECK(describe(r.recorder.inputs)[6] == "7 TimerFired@15");
    CHECK(describe(r.recorder.inputs)[7] == "8 TradeTick@15");
    CHECK(describe(r.recorder.inputs)[8] == "9 NodeLifecycle@15");
  }

  TEST_CASE("a failing callback becomes a StrategyError input and halts the strategy") {
    Echo echo{nullptr};
    echo.fail_trade = 1;
    const Run r =
        run_echo({{key(10, 1, 1), trade(10, 1000)}, {key(20, 1, 2), trade(20, 1000)}}, echo);
    CHECK(describe(r.recorder.inputs)[4] == "5 TradeTick@10");
    CHECK(describe(r.recorder.inputs)[5] == "6 StrategyError@10");
    const auto& error = std::get<md::StrategyError>(r.recorder.inputs[5].event);
    CHECK(error.strategy_index == 0);
    CHECK(error.message_hash != 0);
    CHECK(r.summary.strategy_errors == 1);
    // on_error ran; the halted strategy sees neither the second trade nor on_stop.
    CHECK(r.calls == std::vector<std::string>{"start@10", "trade@10", "error"});
    CHECK(r.summary.data_events == 2);
    CHECK_FALSE(r.summary.halted);
  }

  TEST_CASE("the halt_node policy stops the run after the failing step") {
    Echo echo{nullptr};
    echo.fail_trade = 1;
    const Run r = run_echo({{key(10, 1, 1), trade(10, 1000)}, {key(20, 1, 2), trade(20, 1000)}},
                           echo, {}, st::ErrorPolicy::HaltNode);
    CHECK(r.summary.halted);
    CHECK(r.summary.data_events == 1);
    CHECK(r.summary.state == md::NodeState::Stopped);
    const auto& stopping = std::get<md::NodeLifecycle>(r.recorder.inputs[6].event);
    CHECK(stopping.reason == md::LifecycleReason::ShutdownRequested);
  }

  TEST_CASE("start and end cut the data; timers due before the end still fire") {
    bt::DriverOptions options;
    options.start = UnixNanos{15};
    options.end = UnixNanos{40};
    Echo echo{nullptr, 30, 20}; // fires at 30 and 50; only 30 is before the end
    const Run r = run_echo({{key(10, 1, 1), trade(10, 1000)},
                            {key(20, 1, 2), trade(20, 1000)},
                            {key(40, 1, 3), trade(40, 1000)}},
                           echo, options);
    CHECK(r.calls == std::vector<std::string>{"start@15", "trade@20", "timer1@30 seq8", "stop@30"});
    CHECK(r.summary.data_events == 1);
    CHECK(r.summary.skipped == 2);
    CHECK(r.summary.first_ts == UnixNanos{15});
    CHECK(r.summary.last_ts == UnixNanos{30});
  }

  TEST_CASE("an empty source still runs the lifecycle") {
    const Run r = run_echo({}, Echo{nullptr});
    CHECK(describe(r.recorder.inputs) ==
          std::vector<std::string>{"1 NodeLifecycle@0", "2 NodeLifecycle@0", "3 NodeLifecycle@0",
                                   "4 NodeLifecycle@0", "5 NodeLifecycle@0", "6 NodeLifecycle@0",
                                   "7 BatchEnd@0"});
    CHECK(r.calls == std::vector<std::string>{"start@0", "stop@0"});
  }
}

TEST_SUITE("property") {
  TEST_CASE("the recording alone reproduces every call and output") {
    jarvis::testkit::for_all([](Gen& gen) {
      std::vector<Keyed> trades;
      std::vector<Keyed> quotes;
      std::uint64_t ts = 1'000;
      for (std::uint64_t i = 1; i <= 60; ++i) {
        ts += gen.below(4) * 250; // equal timestamps are common
        const auto px = static_cast<std::int64_t>(1000 + gen.below(5));
        if (gen.coin()) {
          trades.push_back(Keyed{key(ts, 1, i), trade(ts, px)});
        } else {
          quotes.push_back(Keyed{key(ts, 2, i), quote(ts, px)});
        }
      }
      Echo echo{nullptr, 1'000 + gen.below(3'000), 100 + gen.below(900)};
      echo.fail_trade = gen.below(4) == 0 ? 1 + gen.below(10) : 0;

      std::vector<std::string> calls;
      echo.log = &calls;
      MemoryRecorder recorder;
      {
        st::StaticStrategySet<Echo> set{echo};
        jarvis::engine::Engine engine{small_config(), set};
        VectorSource a{trades};
        VectorSource b{quotes};
        bt::MergeSource<VectorSource> merge{2};
        REQUIRE(merge.add(a) == Status::Ok);
        REQUIRE(merge.add(b) == Status::Ok);
        bt::Driver driver{engine, merge, recorder};
        bt::RunSummary summary;
        REQUIRE(driver.run(summary) == Status::Ok);
        REQUIRE(summary.inputs == recorder.inputs.size());
      }
      // Keys are strictly increasing in seq, and never go back in ts.
      for (std::size_t i = 1; i < recorder.inputs.size(); ++i) {
        REQUIRE(recorder.inputs[i].key.seq == recorder.inputs[i - 1].key.seq + 1);
        REQUIRE(!(recorder.inputs[i].key.ts < recorder.inputs[i - 1].key.ts));
      }

      // Replay: step the recorded inputs into a fresh engine, with no driver.
      std::vector<std::string> replay_calls;
      echo.log = &replay_calls;
      echo.trades = 0;
      MemoryRecorder replayed;
      st::StaticStrategySet<Echo> set{echo};
      jarvis::engine::Engine engine{small_config(), set};
      for (const Keyed& input : recorder.inputs) {
        REQUIRE(engine.step(input.key, input.event) == Status::Ok);
        REQUIRE(engine.flush_outputs(replayed) == Status::Ok);
        engine.clear_failures(); // consumed by the recorded StrategyError inputs
      }
      CHECK(replay_calls == calls);
      REQUIRE(replayed.outputs.size() == recorder.outputs.size());
      for (std::size_t i = 0; i < replayed.outputs.size(); ++i) {
        REQUIRE(encode(replayed.outputs[i]) == encode(recorder.outputs[i]));
      }
    });
  }
}

TEST_SUITE("zero-alloc") {
  TEST_CASE("the driver loop does not allocate once warmed up") {
    std::vector<Keyed> data;
    for (std::uint64_t i = 1; i <= 3'000; ++i) {
      data.push_back(Keyed{key(i / 3, 1, i), trade(i / 3, 1000)});
    }
    std::uint64_t trades = 0;
    st::StaticStrategySet<Counter> set{Counter{&trades}};
    jarvis::engine::Engine engine{small_config(), set};
    VectorSource source{std::move(data)};
    CountingRecorder recorder;
    bt::Driver driver{engine, source, recorder};
    bt::RunSummary summary;
    const jarvis::testkit::AllocationScope scope;
    REQUIRE(driver.run(summary) == Status::Ok);
    CHECK(scope.allocations() == 0);
    CHECK(trades == 3'000);
    CHECK(summary.batches == 1'001);
  }
}

namespace {

class LivePushSource {
public:
  void push(EventKey key, const md::Event& event) { items_.push_back(Keyed{key, event}); }
  Status next(EventKey& key, md::Event& event) {
    if (index_ >= items_.size()) {
      return Status::WouldBlock;
    }
    key = items_[index_].key;
    event = items_[index_].event;
    ++index_;
    return Status::Ok;
  }

private:
  std::vector<Keyed> items_;
  std::size_t index_ = 0;
};

// A virtual clock stepping by 250 ns when idle, jumping to arrivals.
class StepPump {
public:
  StepPump(std::vector<Keyed> arrivals, LivePushSource& source, std::uint64_t start,
           std::uint64_t stop)
      : arrivals_{std::move(arrivals)}, source_{&source}, now_{start}, stop_{stop} {}
  [[nodiscard]] UnixNanos now() const { return UnixNanos{now_}; }
  Status pump(UnixNanos now) {
    while (next_ < arrivals_.size() && !(now < arrivals_[next_].key.ts)) {
      Keyed k = arrivals_[next_++];
      k.key.ts = now;
      source_->push(k.key, k.event);
    }
    return Status::Ok;
  }
  Status idle(UnixNanos /*now*/) {
    std::uint64_t next = now_ + 250;
    if (next_ < arrivals_.size() && arrivals_[next_].key.ts.value() < next) {
      next = arrivals_[next_].key.ts.value();
    }
    now_ = next;
    return Status::Ok;
  }
  [[nodiscard]] bool stop_requested() const { return now_ >= stop_; }

private:
  std::vector<Keyed> arrivals_;
  LivePushSource* source_;
  std::uint64_t now_;
  std::uint64_t stop_;
  std::size_t next_ = 0;
};

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("in real time, timers fire once the clock reaches them, between arrivals") {
    const std::vector<Keyed> data = {{key(1000, 1, 1), trade(1000, 1000)},
                                     {key(2000, 1, 2), trade(2000, 1001)},
                                     {key(5000, 1, 3), trade(5000, 1002)}};
    Echo echo;
    echo.timer_at = 1500;
    echo.period = 1000;
    std::vector<std::string> live_calls;
    echo.log = &live_calls;
    st::StaticStrategySet<Echo> set{echo};
    jarvis::engine::Engine engine{small_config(), set};
    LivePushSource source;
    MemoryRecorder recorder;
    bt::Driver driver{engine, source, recorder};
    StepPump pump{data, source, 1000, 5600};
    bt::RunSummary summary;
    REQUIRE(driver.run_realtime(pump, summary) == Status::Ok);

    bt::DriverOptions options;
    options.end = UnixNanos{5600};
    const Run back = run_echo(data, echo, options);
    // The same calls in the same order; only the stop comes at a different time.
    REQUIRE(live_calls.size() == back.calls.size());
    for (std::size_t i = 0; i + 1 < live_calls.size(); ++i) {
      CHECK(live_calls[i] == back.calls[i]);
    }
    CHECK(summary.timers == 5); // 1500, 2500, 3500, 4500, 5500
    CHECK(summary.data_events == 3);
    CHECK(summary.state == md::NodeState::Stopped);
  }

  TEST_CASE("awaiting sync, the node runs once reconciled and degrades while the stream is down") {
    const auto stream = [](std::uint64_t ts, bool up) {
      md::ConnectionStatus c;
      c.kind = md::ConnectionKind::UserStream;
      c.up = up;
      c.ts_init = UnixNanos{ts};
      return md::Event{c};
    };
    const auto snapshot = [](std::uint64_t ts) {
      md::VenueSnapshot v;
      v.ts_snapshot = UnixNanos{ts - 1};
      v.ts_init = UnixNanos{ts};
      return md::Event{v};
    };
    const std::vector<Keyed> arrivals = {
        {key(1000, 1, 1), trade(1000, 1000)},   {key(2000, 2, 2), stream(2000, true)},
        {key(3000, 2, 3), snapshot(3000)},      {key(4000, 1, 4), trade(4000, 1001)},
        {key(5000, 2, 5), stream(5000, false)}, {key(6000, 2, 6), stream(6000, true)},
        {key(7000, 2, 7), snapshot(7000)},      {key(8000, 1, 8), trade(8000, 1002)}};
    std::vector<std::string> calls;
    Echo echo;
    echo.log = &calls;
    st::StaticStrategySet<Echo> set{echo};
    jarvis::engine::Engine engine{small_config(), set};
    LivePushSource source;
    MemoryRecorder recorder;
    bt::DriverOptions options;
    options.await_sync = true;
    bt::Driver driver{engine, source, recorder, options};
    StepPump pump{arrivals, source, 500, 8500};
    bt::RunSummary summary;
    REQUIRE(driver.run_realtime(pump, summary) == Status::Ok);

    std::vector<std::string> moves;
    for (const Keyed& k : recorder.inputs) {
      if (const auto* lc = std::get_if<md::NodeLifecycle>(&k.event)) {
        moves.push_back(std::string{md::to_string(lc->to)} + "@" +
                        std::to_string(k.key.ts.value()));
      }
    }
    CHECK(moves == std::vector<std::string>{"WIRED@500", "STARTING@500", "SYNCING@500",
                                            "RUNNING@3000", "DEGRADED@5000", "SYNCING@6000",
                                            "RUNNING@7000", "STOPPING@8500", "STOPPED@8500"});
    // The trade before the sync reaches nobody: strategies start when the node runs.
    CHECK(calls == std::vector<std::string>{"start@3000", "trade@4000", "trade@8000", "stop@8500"});
    CHECK(engine.kernel().trading.risk.trading_state() == md::TradingState::Active);
  }

  TEST_CASE("the sync gate follows the account's phase") {
    using jarvis::engine::sync_move;
    using ex = jarvis::execution::SyncPhase;
    using md::LifecycleReason;
    using md::NodeState;
    CHECK(sync_move(NodeState::Syncing, ex::Local, false) == LifecycleReason::Synced);
    CHECK(sync_move(NodeState::Syncing, ex::Local, true) == std::nullopt);
    CHECK(sync_move(NodeState::Syncing, ex::Buffering, true) == std::nullopt);
    CHECK(sync_move(NodeState::Syncing, ex::Synced, true) == LifecycleReason::Synced);
    CHECK(sync_move(NodeState::Syncing, ex::Disconnected, true) == LifecycleReason::HealthLost);
    CHECK(sync_move(NodeState::Running, ex::Local, false) == std::nullopt);
    CHECK(sync_move(NodeState::Running, ex::Synced, true) == std::nullopt);
    CHECK(sync_move(NodeState::Running, ex::Disconnected, true) == LifecycleReason::HealthLost);
    CHECK(sync_move(NodeState::Running, ex::Buffering, false) == LifecycleReason::HealthLost);
    CHECK(sync_move(NodeState::Degraded, ex::Disconnected, true) == std::nullopt);
    CHECK(sync_move(NodeState::Degraded, ex::Buffering, true) == LifecycleReason::HealthRestored);
    CHECK(sync_move(NodeState::Degraded, ex::Synced, true) == LifecycleReason::HealthRestored);
    CHECK(sync_move(NodeState::Stopping, ex::Disconnected, true) == std::nullopt);
  }
}
