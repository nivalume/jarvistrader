// Market maker: quotes both sides at the touch (docs/plan.md M3). For tests and soak runs only.
// The C++ twin of examples/py/mm_quote.py: the same decisions in the same order, so for the
// same data both write byte-identical run logs (tools/m3_acceptance.sh checks it).
//
//   build/rel/bin/pegged_mm --config examples/config/mm_quote.toml --env backtest
//   build/rel/bin/pegged_mm --replay runs/mm01/<run>

#include <cstdint>
#include <optional>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/node/node_main.hpp"

#if defined(JARVIS_EXAMPLE_LIVE)
#include "jarvis/live/live_main.hpp"
#endif
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"

namespace {

using jarvis::core::Status;
namespace m = jarvis::model;
namespace st = jarvis::strategy;

bool is_open(m::OrderStatus s) {
  return s == m::OrderStatus::Submitted || s == m::OrderStatus::Accepted ||
         s == m::OrderStatus::Triggered || s == m::OrderStatus::PendingUpdate ||
         s == m::OrderStatus::PendingCancel || s == m::OrderStatus::PartiallyFilled;
}

bool is_pending(m::OrderStatus s) {
  return s == m::OrderStatus::PendingUpdate || s == m::OrderStatus::PendingCancel;
}

struct PeggedMM {
  m::InstrumentId instrument;
  m::Quantity size;
  std::int64_t max_raw = 0;
  std::int64_t skew_ticks = 1;
  std::int64_t sample_ms = 1000;
  std::optional<m::ClientOrderId> bid;
  std::optional<m::ClientOrderId> ask;

  // What a snapshot keeps (docs/architecture.md section 16.3): the working orders. The rest
  // comes from the parameters.
  template <typename Ar> void state(Ar& ar) { ar(bid, ask); }

  static Status create(const jarvis::node::StrategyParams& p, PeggedMM& out) {
    std::string_view instrument = "BTCUSDT-PERP.BINANCE";
    std::string_view size = "0.010";
    std::string_view max_position = "0.050";
    Status s = p.get_or<std::string_view>("instrument", instrument, instrument);
    s = jarvis::core::ok(s) ? m::InstrumentId::parse(instrument, out.instrument) : s;
    s = jarvis::core::ok(s) ? p.get_or<std::string_view>("size", size, size) : s;
    s = jarvis::core::ok(s) ? m::Quantity::parse(size, out.size) : s;
    s = jarvis::core::ok(s) ? p.get_or<std::string_view>("max_position", max_position, max_position)
                            : s;
    m::Quantity max;
    s = jarvis::core::ok(s) ? m::Quantity::parse(max_position, max) : s;
    out.max_raw = static_cast<std::int64_t>(max.raw());
    s = jarvis::core::ok(s) ? p.get_or<std::int64_t>("skew_ticks", 1, out.skew_ticks) : s;
    return jarvis::core::ok(s) ? p.get_or<std::int64_t>("sample_ms", 1000, out.sample_ms) : s;
  }

  [[nodiscard]] Status on_start(st::Context& ctx) const {
    return ctx.subscribe_quotes(
        instrument, jarvis::data::Cadence::sampled_ms(static_cast<std::uint64_t>(sample_ms)));
  }

  Status on_quote(st::Context& ctx, const m::QuoteTick& q) {
    m::Instrument definition;
    if (!ctx.instrument(instrument, definition)) {
      return Status::Ok;
    }
    const m::InstrumentCommon& c = m::common(definition);
    const std::int64_t tick = c.price_increment.raw();
    st::PositionView position;
    const std::int64_t pos = ctx.position(instrument, position) ? position.signed_qty.raw() : 0;
    const std::int64_t skew = skew_ticks * tick;
    const std::int64_t bid_px = q.bid_price.raw() - (pos > 0 ? skew : 0);
    const std::int64_t ask_px = q.ask_price.raw() + (pos < 0 ? skew : 0);
    const auto qty = static_cast<std::int64_t>(size.raw());
    Status s = side(ctx, bid, m::OrderSide::Buy, bid_px, c.price_precision, pos + qty <= max_raw);
    if (jarvis::core::ok(s)) {
      s = side(ctx, ask, m::OrderSide::Sell, ask_px, c.price_precision, pos - qty >= -max_raw);
    }
    return s;
  }

  // One working order on `side` at `price_raw` while `active`, else none.
  Status side(st::Context& ctx, std::optional<m::ClientOrderId>& order, m::OrderSide which,
              std::int64_t price_raw, std::uint8_t precision, bool active) const {
    st::OrderView view;
    const bool working = order && ctx.order(*order, view) && is_open(view.status);
    if (!active) {
      if (working && view.status != m::OrderStatus::PendingCancel) {
        const Status s = ctx.cancel(*order);
        if (!jarvis::core::ok(s) && s != Status::InvalidState) {
          return s;
        }
      }
      if (!working) {
        order.reset();
      }
      return Status::Ok;
    }
    m::Price price;
    Status s = m::Price::from_raw(price_raw, precision, price);
    if (!jarvis::core::ok(s)) {
      return s;
    }
    if (!working) {
      m::ClientOrderId id;
      s = ctx.submit(ctx.limit(instrument, which, size, price, m::TimeInForce::Gtc, true), id);
      order = id;
      return s;
    }
    if (!is_pending(view.status) && view.price && view.price->raw() != price_raw) {
      s = ctx.modify(*order, std::nullopt, price);
      return s == Status::InvalidState ? Status::Ok : s;
    }
    return Status::Ok;
  }
};

} // namespace

JARVIS_REGISTER_STRATEGY(PeggedMM, "PeggedMM");

#if defined(JARVIS_EXAMPLE_LIVE)
int main(int argc, char** argv) { return jarvis::live_node_main<PeggedMM>(argc, argv); }
#else
int main(int argc, char** argv) { return jarvis::node_main<PeggedMM>(argc, argv); }
#endif
