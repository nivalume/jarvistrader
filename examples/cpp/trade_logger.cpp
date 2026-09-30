// Trade logger: records market activity and never trades (docs/plan.md M2). The C++ twin of
// examples/py/trade_logger.py; for the same data both write byte-identical run logs.
//
//   build/rel/bin/trade_logger --config examples/config/trade_logger.toml --env backtest
//   build/rel/bin/trade_logger --replay runs/trade_logger/<run>

#include <cstdint>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/node/node_main.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"

namespace {

using jarvis::core::Status;
using jarvis::core::TimerKey;
using jarvis::core::UnixNanos;
namespace m = jarvis::model;

constexpr std::uint64_t kScale = 1'000'000'000;

// Raw value at 10^9 as a Decimal with nine decimals (the text the Python twin records).
m::Decimal fixed9(std::int64_t raw) {
  m::Decimal d;
  static_cast<void>(m::Decimal::from_raw(raw, 9, d));
  return d;
}

struct TradeLogger {
  m::InstrumentId instrument;
  std::int64_t sample_ms = 1000;
  std::int64_t window_s = 60;
  std::uint64_t trades = 0;
  std::uint64_t volume_raw = 0;

  // What a snapshot keeps (docs/architecture.md section 16.3): the counts so far.
  template <typename Ar> void state(Ar& ar) { ar(trades, volume_raw); }

  static Status create(const jarvis::node::StrategyParams& p, TradeLogger& out) {
    std::string_view instrument = "BTCUSDT-PERP.BINANCE";
    Status s = p.get_or<std::string_view>("instrument", instrument, instrument);
    if (jarvis::core::ok(s)) {
      s = m::InstrumentId::parse(instrument, out.instrument);
    }
    if (jarvis::core::ok(s)) {
      s = p.get_or<std::int64_t>("sample_ms", 1000, out.sample_ms);
    }
    if (jarvis::core::ok(s)) {
      s = p.get_or<std::int64_t>("window_s", 60, out.window_s);
    }
    return s;
  }

  Status on_start(jarvis::strategy::Context& ctx) const {
    Status s = ctx.subscribe_trades(instrument);
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_quotes(
          instrument, jarvis::data::Cadence::sampled_ms(static_cast<std::uint64_t>(sample_ms)));
    }
    const std::uint64_t window = static_cast<std::uint64_t>(window_s) * kScale;
    if (jarvis::core::ok(s)) {
      s = ctx.set_timer(1, UnixNanos{(ctx.now().value() / window + 1) * window},
                        jarvis::core::DurationNanos{window});
    }
    return s;
  }

  void on_trade(jarvis::strategy::Context& /*ctx*/, const m::TradeTick& t) {
    ++trades;
    volume_raw += t.size.raw();
  }

  static Status on_quote(jarvis::strategy::Context& ctx, const m::QuoteTick& q) {
    return ctx.record("mid", fixed9((q.bid_price.raw() + q.ask_price.raw()) / 2));
  }

  Status on_timer(jarvis::strategy::Context& ctx, TimerKey /*key*/, UnixNanos /*deadline*/) {
    m::Decimal count;
    static_cast<void>(m::Decimal::from_raw(static_cast<std::int64_t>(trades * kScale), 0, count));
    Status s = ctx.record("trades", count);
    if (jarvis::core::ok(s)) {
      s = ctx.record("volume", fixed9(static_cast<std::int64_t>(volume_raw)));
    }
    trades = 0;
    volume_raw = 0;
    return s;
  }
};

} // namespace

JARVIS_REGISTER_STRATEGY(TradeLogger, "TradeLogger");

int main(int argc, char** argv) { return jarvis::node_main<TradeLogger>(argc, argv); }
