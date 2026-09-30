// Report-only benchmarks for EngineState snapshots (docs/architecture.md sections 16.3 and 17.3).
// A snapshot is taken every persistence.snapshot_every inputs, off the per-input path, so these
// are reported, not gated. The kernel has the default capacities (64 instruments, 8 strategies,
// 4096 orders) and a state a market maker would have: one perpetual with a 200-level book per
// side, trade and quote subscriptions, and 200 orders in the OMS (100 still open).
//
//   snapshot/save_state   Engine::save_state into a reused buffer
//   snapshot/load_state   Engine::load_state of those bytes into a second engine

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "jarvis/core/event_key.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"

namespace {

namespace m = jarvis::model;
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
  return p;
}

constexpr std::int64_t kMid = 650'000;

// Subscribes to the book, trades and quotes, and places 200 limit orders away from the market.
struct Maker {
  std::uint64_t events = 0;
  std::uint32_t counter = 0;

  template <typename Ar> void state(Ar& ar) { ar(events, counter); }

  Status on_start(st::Context& ctx) {
    Status s = ctx.subscribe_book(btc());
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_trades(btc());
    }
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_quotes(btc(), jarvis::data::Cadence::conflated());
    }
    for (int i = 0; i < 200 && jarvis::core::ok(s); ++i) {
      const bool buy = (i & 1) == 0;
      m::ClientOrderId id;
      s = ctx.submit(ctx.limit(btc(), buy ? m::OrderSide::Buy : m::OrderSide::Sell, qty(10),
                               px(buy ? kMid - 300 - i : kMid + 300 + i)),
                     id);
      ++counter;
    }
    return s;
  }
  void on_book(st::Context& /*ctx*/, const jarvis::data::BookView& /*b*/) { ++events; }
  void on_trade(st::Context& /*ctx*/, const m::TradeTick& /*t*/) { ++events; }
};

using MakerEngine = jarvis::engine::Engine<st::StaticStrategySet<Maker>>;

m::OrderBookDelta delta(m::OrderSide side, std::int64_t price_tenths, std::uint64_t sequence) {
  m::OrderBookDelta d;
  d.instrument_id = btc();
  d.action = m::BookAction::Add;
  d.order.side = side;
  d.order.price = px(price_tenths);
  d.order.size = qty(1000);
  d.sequence = sequence;
  d.ts_event = UnixNanos{sequence};
  d.ts_init = UnixNanos{sequence};
  return d;
}

// Steps the engine into the state described above.
void populate(MakerEngine& engine) {
  std::uint64_t seq = 0;
  const auto step = [&engine, &seq](const m::Event& e) {
    static_cast<void>(engine.step(EventKey{UnixNanos{1 + seq}, 1, seq + 1}, e));
    ++seq;
    engine.clear_outputs();
    engine.clear_failures();
  };
  step(m::Event{perpetual()});
  step(m::Event{m::NodeLifecycle{m::NodeState::Syncing, m::NodeState::Running,
                                 m::LifecycleReason::Synced, UnixNanos{2}}});
  for (std::int64_t level = 1; level <= 200; ++level) {
    const auto n = static_cast<std::uint64_t>(level);
    const std::array<m::OrderBookDelta, 2> pair{
        delta(m::OrderSide::Buy, kMid - level, 2 * n),
        delta(m::OrderSide::Sell, kMid + level, (2 * n) + 1)};
    m::OrderBookDeltas deltas;
    static_cast<void>(m::OrderBookDeltas::create(pair, deltas));
    step(m::Event{deltas});
  }
  // The venue accepts the orders; half of them are canceled again.
  const MakerEngine& view = engine;
  const jarvis::execution::Oms& oms = view.kernel().trading.oms;
  for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
    const jarvis::execution::OrderRecord& r = oms.at(i);
    m::OrderAccepted accepted;
    accepted.header.trader_id = view.kernel().trading.trader_id;
    accepted.header.strategy_id = view.kernel().trading.strategy_ids[r.strategy];
    accepted.header.instrument_id = r.instrument_id;
    accepted.header.client_order_id = r.client_order_id;
    static_cast<void>(m::VenueOrderId::from(std::to_string(1000 + i), accepted.venue_order_id));
    accepted.account_id = view.kernel().trading.account_id;
    step(m::Event{accepted});
    if ((i & 1U) == 1) {
      m::OrderCanceled canceled;
      canceled.header = accepted.header;
      canceled.venue_order_id = accepted.venue_order_id;
      canceled.account_id = accepted.account_id;
      step(m::Event{canceled});
    }
  }
}

void save_state(benchmark::State& state) {
  st::StaticStrategySet<Maker> set{Maker{}};
  MakerEngine engine{st::KernelConfig{}, set};
  populate(engine);
  std::vector<std::byte> bytes;
  for ([[maybe_unused]] auto _ : state) {
    bytes.clear();
    Status s = engine.save_state(bytes);
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(bytes.data());
  }
  state.counters["bytes"] = static_cast<double>(bytes.size());
}

void load_state(benchmark::State& state) {
  st::StaticStrategySet<Maker> set{Maker{}};
  MakerEngine engine{st::KernelConfig{}, set};
  populate(engine);
  std::vector<std::byte> bytes;
  static_cast<void>(engine.save_state(bytes));
  st::StaticStrategySet<Maker> other_set{Maker{}};
  MakerEngine other{st::KernelConfig{}, other_set};
  for ([[maybe_unused]] auto _ : state) {
    Status s = other.load_state(bytes);
    benchmark::DoNotOptimize(s);
  }
  state.counters["bytes"] = static_cast<double>(bytes.size());
}

} // namespace

BENCHMARK(save_state)->Name("snapshot/save_state")->Unit(benchmark::kMicrosecond);
BENCHMARK(load_state)->Name("snapshot/load_state")->Unit(benchmark::kMicrosecond);
