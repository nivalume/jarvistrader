// Gating benchmarks for the engine and the order book (docs/architecture.md 7.8 and 17.3).
//
//   step/trade_to_strategy    Engine::step of a TradeTick delivered to one C++ strategy that
//                             subscribed with Cadence::Every (routing, delivery, callback)
//   step/trade_with_feature   the same trade also updating an EMA feature delivered to the
//                             strategy, with the FeatureUpdate output
//   book/apply_l2_delta       OrderBook::apply of one L2 update near the top of a book holding
//                             200 levels per side

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "jarvis/core/event_key.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/data/features.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
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

m::TradeTick trade(std::uint64_t ts, std::int64_t price_tenths) {
  m::TradeTick t;
  t.instrument_id = btc();
  static_cast<void>(m::Price::from_raw(price_tenths * 100'000'000, 1, t.price));
  static_cast<void>(m::Quantity::from_raw(10'000'000, 3, t.size));
  t.aggressor_side = m::AggressorSide::Buy;
  static_cast<void>(m::TradeId::from("1", t.trade_id));
  t.ts_event = UnixNanos{ts};
  t.ts_init = UnixNanos{ts};
  return t;
}

struct Sink {
  std::uint64_t seen = 0;
  bool with_feature = false;
  Status on_start(st::Context& ctx) const {
    Status s = ctx.subscribe_trades(btc());
    if (jarvis::core::ok(s) && with_feature) {
      m::FeatureId id = 0;
      s = ctx.feature(jarvis::data::FeatureSpec{jarvis::data::FeatureKind::Ema, btc(), 20},
                      jarvis::data::Cadence::every(), id);
    }
    return s;
  }
  void on_trade(st::Context& /*ctx*/, const m::TradeTick& t) {
    seen += static_cast<std::uint64_t>(t.price.raw());
  }
  void on_feature(st::Context& /*ctx*/, m::FeatureId /*id*/, m::Decimal v, UnixNanos /*ts*/) {
    seen += static_cast<std::uint64_t>(v.raw());
  }
};

st::KernelConfig config() {
  st::KernelConfig c;
  c.instruments = 8;
  c.strategies = 4;
  c.book_window_levels = 1024;
  c.book_overflow_levels = 64;
  return c;
}

void run_trades(benchmark::State& state, bool with_feature) {
  st::StaticStrategySet<Sink> set{Sink{0, with_feature}};
  jarvis::engine::Engine engine{config(), set};
  std::uint64_t seq = 0;
  const m::NodeLifecycle running{m::NodeState::Syncing, m::NodeState::Running,
                                 m::LifecycleReason::Synced, UnixNanos{1}};
  static_cast<void>(engine.step(EventKey{UnixNanos{1}, 0, ++seq}, m::Event{running}));
  std::array<m::Event, 16> events{};
  for (std::size_t i = 0; i < events.size(); ++i) {
    events[i] = m::Event{trade(10 + i, 650'000 + static_cast<std::int64_t>(i % 5))};
  }
  std::size_t i = 0;
  for ([[maybe_unused]] auto _ : state) {
    const m::Event& e = events[i++ & 15U];
    Status s = engine.step(EventKey{UnixNanos{10 + seq}, 1, ++seq}, e);
    benchmark::DoNotOptimize(s);
    engine.clear_outputs();
  }
  benchmark::DoNotOptimize(set.get<0>().seen);
}

void trade_to_strategy(benchmark::State& state) { run_trades(state, false); }
void trade_with_feature(benchmark::State& state) { run_trades(state, true); }

m::OrderBookDelta delta(m::BookAction action, m::OrderSide side, std::int64_t price_tenths,
                        std::uint64_t size_thousandths, std::uint64_t sequence) {
  m::OrderBookDelta d;
  d.instrument_id = btc();
  d.action = action;
  d.order.side = side;
  static_cast<void>(m::Price::from_raw(price_tenths * 100'000'000, 1, d.order.price));
  static_cast<void>(m::Quantity::from_raw(size_thousandths * 1'000'000, 3, d.order.size));
  d.sequence = sequence;
  d.ts_event = UnixNanos{sequence};
  d.ts_init = UnixNanos{sequence};
  return d;
}

void apply_l2_delta(benchmark::State& state) {
  jarvis::data::BookConfig c;
  static_cast<void>(m::Price::from_raw(100'000'000, 1, c.tick));
  c.size_precision = 3;
  c.window_levels = 4096;
  jarvis::data::OrderBook book{c};
  constexpr std::int64_t kMid = 650'000;
  std::uint64_t sequence = 0;
  for (std::int64_t level = 1; level <= 200; ++level) {
    static_cast<void>(
        book.apply(delta(m::BookAction::Add, m::OrderSide::Buy, kMid - level, 1000, ++sequence)));
    static_cast<void>(
        book.apply(delta(m::BookAction::Add, m::OrderSide::Sell, kMid + level, 1000, ++sequence)));
  }
  std::array<m::OrderBookDelta, 32> updates{};
  for (std::size_t i = 0; i < updates.size(); ++i) {
    const auto level = static_cast<std::int64_t>(1 + (i * 7) % 20);
    const bool bid = (i & 1U) == 0;
    updates[i] = delta(m::BookAction::Update, bid ? m::OrderSide::Buy : m::OrderSide::Sell,
                       bid ? kMid - level : kMid + level, 500 + i, 0);
  }
  std::size_t i = 0;
  for ([[maybe_unused]] auto _ : state) {
    m::OrderBookDelta& d = updates[i++ & 31U];
    d.sequence = ++sequence;
    Status s = book.apply(d);
    benchmark::DoNotOptimize(s);
    benchmark::ClobberMemory();
  }
}

} // namespace

BENCHMARK(trade_to_strategy)->Name("step/trade_to_strategy");
BENCHMARK(trade_with_feature)->Name("step/trade_with_feature");
BENCHMARK(apply_l2_delta)->Name("book/apply_l2_delta");
