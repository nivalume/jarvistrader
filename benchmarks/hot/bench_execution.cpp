// Gating benchmarks for orders and risk (docs/architecture.md sections 8, 10 and 17.3).
//
//   oms/apply_order_event    execution::apply_order_event of one venue event on an accepted
//                            order: ClientOrderId lookup and the state machine (a pending cancel
//                            and its rejection, alternately, so the order returns to ACCEPTED)
//   risk/gate_a              the Gate A rules on one limit order
//   risk/gate_b              the Gate B rules on the same order (without the rate limit)
//   sim/match_top_of_book    SimulatedExchange::on_data of a quote with 20 resting orders on
//                            the book (none crossed), top-of-book fill model
//   sim/match_queue_position the same with the queue-position model, alternating quotes and
//                            trades at a resting price (the queue ahead shrinks, never to zero)
//   step/quote_to_command    Engine::step of a quote whose callback submits a limit order through
//                            both gates into the OMS (a SubmitOrder output), followed by the
//                            venue's OrderRejected that closes it, so the OMS stays at steady
//                            state; the time covers both steps

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "jarvis/backtest/matching/sim_exchange.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/execution/execution_engine.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/risk/gates.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"

namespace {

namespace m = jarvis::model;
namespace ex = jarvis::execution;
namespace r = jarvis::risk;
namespace st = jarvis::strategy;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

m::InstrumentId btc() {
  m::InstrumentId id;
  static_cast<void>(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id));
  return id;
}
m::Price px(std::int64_t tenths) {
  m::Price p;
  static_cast<void>(m::Price::from_raw(tenths * 100'000'000, 1, p));
  return p;
}
m::Quantity qty(std::uint64_t thousandths) {
  m::Quantity q;
  static_cast<void>(m::Quantity::from_raw(thousandths * 1'000'000, 3, q));
  return q;
}

m::CryptoPerpetual perpetual() {
  m::CryptoPerpetual p;
  m::InstrumentCommon& c = p.common;
  c.id = btc();
  static_cast<void>(m::Symbol::from("BTCUSDT", c.raw_symbol));
  static_cast<void>(m::Currency::builtin("BTC", c.base_currency.emplace()));
  static_cast<void>(m::Currency::builtin("USDT", c.quote_currency));
  c.settlement_currency = c.quote_currency;
  c.price_precision = 1;
  c.size_precision = 3;
  c.price_increment = px(1);
  c.size_increment = qty(1);
  static_cast<void>(m::Quantity::from_raw(1'000'000'000, 0, c.multiplier));
  static_cast<void>(m::Decimal::from_raw(50'000'000, 2, c.margin_init));
  static_cast<void>(m::Decimal::from_raw(25'000'000, 3, c.margin_maint));
  static_cast<void>(m::Money::parse("5 USDT", c.min_notional.emplace()));
  return p;
}

void apply_order_event(benchmark::State& state) {
  ex::Oms oms{4096, 16384, 8};
  // A realistic table: 1000 live orders, the measured one among them.
  for (int i = 0; i < 1000; ++i) {
    ex::OrderRecord rec;
    static_cast<void>(m::ClientOrderId::from("bench-" + std::to_string(i), rec.client_order_id));
    rec.price = px(650'000);
    rec.state = ex::OrderState{qty(10)};
    std::uint32_t index = 0;
    static_cast<void>(oms.create(rec, index));
    static_cast<void>(oms.apply(index, ex::OrderEventKind::Submitted));
    static_cast<void>(oms.apply(index, ex::OrderEventKind::Accepted));
  }
  m::OrderPendingCancel pending;
  static_cast<void>(m::ClientOrderId::from("bench-500", pending.header.client_order_id));
  m::OrderCancelRejected rejected;
  rejected.header = pending.header;
  const std::array<m::OrderEvent, 2> events = {m::OrderEvent{pending}, m::OrderEvent{rejected}};
  std::size_t i = 0;
  for ([[maybe_unused]] auto _ : state) {
    std::uint32_t index = 0;
    ex::EventOutcome outcome = ex::apply_order_event(oms, events[i++ & 1U], index);
    benchmark::DoNotOptimize(outcome);
  }
}

struct GateFixture {
  m::CryptoPerpetual instrument = perpetual();
  r::RiskConfig config;
  r::OrderCheck check;
  GateFixture() {
    static_cast<void>(m::Money::parse("100000 USDT", config.max_order_notional.emplace()));
    static_cast<void>(m::Money::parse("500000 USDT", config.max_position_notional.emplace()));
    config.price_band_bps = 200;
    config.max_open_orders = 50;
    check.instrument = &instrument.common;
    check.quantity = qty(10);
    check.price = px(649'990);
    check.reference = px(650'000);
    check.position_raw = 5'000'000;
    check.open.orders = 4;
    check.open.buy_raw = 20'000'000;
    check.margin_known = true;
    check.available_raw = 1'000'000'000'000;
    check.required_raw = 32'500'000'000;
  }
};

void gate_a(benchmark::State& state) {
  const GateFixture f;
  const r::GateState s{&f.config, m::TradingState::Active, true, true};
  for ([[maybe_unused]] auto _ : state) {
    std::string_view denied = r::run_gate<r::GateA>(f.check, s);
    benchmark::DoNotOptimize(denied);
  }
}

void gate_b(benchmark::State& state) {
  const GateFixture f;
  const r::GateState s{&f.config, m::TradingState::Active, true, true};
  for ([[maybe_unused]] auto _ : state) {
    std::string_view denied = r::run_gate<r::GateB>(f.check, s);
    benchmark::DoNotOptimize(denied);
  }
}

template <jarvis::backtest::FillModel Model> void match(benchmark::State& state) {
  jarvis::backtest::SimConfig config;
  config.fill_model = Model;
  config.instruments = 4;
  config.orders = 64;
  jarvis::backtest::SimulatedExchange sim{config};
  static_cast<void>(sim.on_data(m::Event{perpetual()}, UnixNanos{1}));
  m::QuoteTick quote;
  quote.instrument_id = btc();
  quote.bid_price = px(649'990);
  quote.ask_price = px(650'000);
  quote.bid_size = qty(1'000'000); // 1000 BTC at the touch: the queue never empties
  quote.ask_size = qty(1'000'000);
  static_cast<void>(sim.on_data(m::Event{quote}, UnixNanos{2}));
  for (int i = 0; i < 10; ++i) {
    for (const m::OrderSide side : {m::OrderSide::Buy, m::OrderSide::Sell}) {
      m::SubmitOrder s;
      static_cast<void>(m::ClientOrderId::from(std::string{side == m::OrderSide::Buy ? "b" : "s"} +
                                                   std::to_string(i),
                                               s.client_order_id));
      s.instrument_id = btc();
      s.order_side = side;
      s.quantity = qty(10);
      s.price = side == m::OrderSide::Buy ? px(649'990 - i) : px(650'000 + i);
      static_cast<void>(sim.on_command(m::Output{s}, UnixNanos{3}));
    }
  }
  m::TradeTick trade;
  trade.instrument_id = btc();
  trade.price = px(649'990);
  trade.size = qty(1);
  trade.aggressor_side = m::AggressorSide::Sell;
  const std::array<m::Event, 2> events = {m::Event{quote}, m::Event{trade}};
  std::uint64_t ts = 10;
  std::size_t i = 0;
  for ([[maybe_unused]] auto _ : state) {
    sim.clear_events();
    const m::Event& e =
        Model == jarvis::backtest::FillModel::QueuePosition ? events[i++ & 1U] : events[0];
    Status s = sim.on_data(e, UnixNanos{++ts});
    benchmark::DoNotOptimize(s);
  }
}

struct Quoter {
  m::ClientOrderId* last = nullptr;
  static Status on_start(st::Context& ctx) { return ctx.subscribe_quotes(btc()); }
  Status on_quote(st::Context& ctx, const m::QuoteTick& q) const {
    return ctx.submit(ctx.limit(q.instrument_id, m::OrderSide::Buy, qty(10), q.bid_price), *last);
  }
};

void quote_to_command(benchmark::State& state) {
  m::ClientOrderId last;
  st::StaticStrategySet<Quoter> set{Quoter{&last}};
  st::KernelConfig config;
  config.instruments = 8;
  config.strategies = 4;
  config.trading.risk.orders_per_10s = 0;
  config.trading.risk.orders_per_minute = 0;
  jarvis::engine::Engine engine{config, set};
  std::uint64_t seq = 0;
  static_cast<void>(engine.step(EventKey{UnixNanos{1}, 0, ++seq}, m::Event{perpetual()}));
  const m::NodeLifecycle running{m::NodeState::Syncing, m::NodeState::Running,
                                 m::LifecycleReason::Synced, UnixNanos{1}};
  static_cast<void>(engine.step(EventKey{UnixNanos{1}, 0, ++seq}, m::Event{running}));
  m::QuoteTick quote;
  quote.instrument_id = btc();
  quote.bid_price = px(649'990);
  quote.ask_price = px(650'000);
  quote.bid_size = qty(1000);
  quote.ask_size = qty(1000);
  m::OrderRejected rejected;
  rejected.header.instrument_id = btc();
  m::Event reject{rejected};
  for ([[maybe_unused]] auto _ : state) {
    const UnixNanos ts{10 + seq};
    quote.ts_event = ts;
    quote.ts_init = ts;
    Status s = engine.step(EventKey{ts, 1, ++seq}, m::Event{quote});
    benchmark::DoNotOptimize(s);
    std::get<m::OrderRejected>(reject).header.client_order_id = last;
    s = engine.step(EventKey{ts, 2, ++seq}, reject);
    benchmark::DoNotOptimize(s);
    engine.clear_outputs();
  }
}

} // namespace

BENCHMARK(apply_order_event)->Name("oms/apply_order_event");
BENCHMARK(gate_a)->Name("risk/gate_a");
BENCHMARK(gate_b)->Name("risk/gate_b");
BENCHMARK(match<jarvis::backtest::FillModel::TopOfBook>)->Name("sim/match_top_of_book");
BENCHMARK(match<jarvis::backtest::FillModel::QueuePosition>)->Name("sim/match_queue_position");
BENCHMARK(quote_to_command)->Name("step/quote_to_command");
