#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/risk/monitors.hpp"
#include "jarvis/risk/rate_limit.hpp"
#include "jarvis/risk/trading_state.hpp"

// The two risk gates (docs/architecture.md sections 9.2 and 10.1). Every rule is a RiskRule:
// it reads one OrderCheck (the order and a snapshot of what it would change) and the gate's
// shared state, and returns the reason code that denies the order, or nothing. A gate is a
// fixed tuple of rules checked in order; the first denial wins. The rate limit spends a token,
// so it runs last, after every other rule passed.
//
//   Gate A (the intent)   TradingState, instrument whitelist, instrument status, position
//                         notional including open orders;
//   Gate B (the order)    TradingState, price filter, lot size, minimum notional, maximum order
//                         notional, price band, open orders, reduce-only, margin, rate limit.
//
// Amounts in a limit are compared only in the limit's currency; a rule whose currency differs
// from the instrument's notional currency does not apply.

namespace jarvis::risk {

namespace reason {

inline constexpr std::string_view kTradingHalted = "TRADING_HALTED";
inline constexpr std::string_view kTradingReducingOnly = "TRADING_REDUCING_ONLY";
inline constexpr std::string_view kInstrumentNotAllowed = "INSTRUMENT_NOT_ALLOWED";
inline constexpr std::string_view kInstrumentNotTrading = "INSTRUMENT_NOT_TRADING";
inline constexpr std::string_view kExposureExceedsLimit = "EXPOSURE_EXCEEDS_LIMIT";
inline constexpr std::string_view kPriceInvalidTick = "PRICE_INVALID_TICK";
inline constexpr std::string_view kPriceOutOfRange = "PRICE_OUT_OF_RANGE";
inline constexpr std::string_view kQuantityInvalidStep = "QUANTITY_INVALID_STEP";
inline constexpr std::string_view kQuantityOutOfRange = "QUANTITY_OUT_OF_RANGE";
inline constexpr std::string_view kNotionalBelowMin = "NOTIONAL_BELOW_MIN";
inline constexpr std::string_view kNotionalExceedsMax = "NOTIONAL_EXCEEDS_MAX_PER_ORDER";
inline constexpr std::string_view kPriceOutsideBand = "PRICE_OUTSIDE_BAND";
inline constexpr std::string_view kOpenOrdersExceeded = "OPEN_ORDERS_EXCEEDED";
inline constexpr std::string_view kRateLimitExceeded = "RATE_LIMIT_EXCEEDED";
inline constexpr std::string_view kReduceOnlyInvalid = "REDUCE_ONLY_INVALID";
inline constexpr std::string_view kMarginInsufficient = "MARGIN_INSUFFICIENT";

} // namespace reason

struct RiskConfig {
  model::TradingState initial_state = model::TradingState::Active;
  std::optional<model::Money> max_order_notional;
  std::optional<model::Money> max_position_notional; // per instrument, open orders included
  std::optional<model::Money> daily_loss_limit;      // soft: Reducing
  std::optional<model::Money> daily_loss_halt;       // hard: Halted and KillSwitch
  std::optional<model::Money> max_drawdown;          // soft
  std::uint32_t price_band_bps = 0;                  // 0: off
  std::uint32_t max_open_orders = 0;                 // per instrument; 0: off
  std::uint32_t orders_per_10s = 250;                // Binance USD-M allows 300; 0: off
  std::uint32_t orders_per_minute = 1000;            // Binance USD-M allows 1200; 0: off
  std::uint32_t margin_ratio_bps = 8000;             // soft at 80% maintenance / equity; 0: off
  bool check_margin = true;
};

// One order as the gates see it, with a snapshot of the state it would change. Built by the
// kernel for every submit and modify.
struct OrderCheck {
  const model::InstrumentCommon* instrument = nullptr;
  std::uint32_t slot = 0;
  std::uint16_t strategy = 0;
  CommandKind kind = CommandKind::Open;
  model::OrderSide side = model::OrderSide::Buy;
  model::OrderType type = model::OrderType::Limit;
  model::Quantity quantity;
  std::optional<model::Price> price;
  bool reduce_only = false;
  core::UnixNanos now;
  std::int64_t position_raw = 0;         // the venue position, signed
  execution::OpenQuantity open;          // the instrument's open orders, before this one
  std::optional<model::Price> reference; // mark price, else last trade
  // Margin in the settlement currency (10^9 raw); only when the account balance is known.
  bool margin_known = false;
  std::int64_t available_raw = 0; // wallet + unrealized - initial margin of positions and orders
  std::int64_t required_raw = 0;  // initial margin of this order
};

// Read-only state the rules consult.
struct GateState {
  const RiskConfig* config = nullptr;
  model::TradingState trading_state = model::TradingState::Active;
  bool instrument_allowed = true;
  bool instrument_trading = true;
};

template <typename R>
concept RiskRule = requires(const R& rule, const OrderCheck& check, const GateState& state) {
  { rule.check(check, state) } -> std::same_as<std::string_view>;
};

namespace detail {

// |quantity| x price in the notional currency, or nothing when it cannot be computed.
[[nodiscard]] constexpr std::optional<model::Money> notional(const model::InstrumentCommon& c,
                                                             std::uint64_t quantity_raw,
                                                             std::uint8_t precision,
                                                             model::Price price) noexcept {
  model::Quantity q;
  model::Money out;
  if (!core::ok(model::Quantity::from_raw(quantity_raw, precision, q)) ||
      !core::ok(model::notional_value(c, q, price, out))) {
    return std::nullopt;
  }
  return out;
}

// a > limit, when both are in the same currency.
[[nodiscard]] constexpr bool exceeds(const std::optional<model::Money>& amount,
                                     const std::optional<model::Money>& limit) noexcept {
  return amount && limit && amount->currency() == limit->currency() &&
         core::magnitude(amount->raw()) > core::magnitude(limit->raw());
}

// The price an order's notional is measured at: its own, else the reference.
[[nodiscard]] constexpr std::optional<model::Price> order_price(const OrderCheck& c) noexcept {
  return c.price ? c.price : c.reference;
}

[[nodiscard]] constexpr bool new_order(const OrderCheck& c) noexcept {
  return c.kind == CommandKind::Open || c.kind == CommandKind::Reduce;
}

} // namespace detail

// ---- rules --------------------------------------------------------------------------------------

struct TradingStateRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    if (allowed(s.trading_state, c.kind)) {
      return {};
    }
    return s.trading_state == model::TradingState::Halted ? reason::kTradingHalted
                                                          : reason::kTradingReducingOnly;
  }
};

struct InstrumentWhitelistRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& /*c*/,
                                                        const GateState& s) noexcept {
    return s.instrument_allowed ? std::string_view{} : reason::kInstrumentNotAllowed;
  }
};

struct InstrumentStatusRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    // A halted market still takes orders that only reduce: they may be the way out.
    return s.instrument_trading || c.kind == CommandKind::Reduce ? std::string_view{}
                                                                 : reason::kInstrumentNotTrading;
  }
};

// |position + open orders on the order's side + the order| at the reference price.
struct IntentNotionalRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    if (!s.config->max_position_notional || c.kind != CommandKind::Open || !c.reference) {
      return {};
    }
    const core::i128 q = c.quantity.raw();
    const core::i128 worst = c.side == model::OrderSide::Buy
                                 ? core::i128{c.position_raw} + c.open.buy_raw + q
                                 : core::i128{c.position_raw} - c.open.sell_raw - q;
    const core::i128 magnitude = worst < 0 ? -worst : worst;
    if (magnitude > static_cast<core::i128>(model::kQuantityRawMax)) {
      return reason::kExposureExceedsLimit;
    }
    const std::optional<model::Money> n = detail::notional(
        *c.instrument, static_cast<std::uint64_t>(magnitude), c.quantity.precision(), *c.reference);
    return detail::exceeds(n, s.config->max_position_notional) ? reason::kExposureExceedsLimit
                                                               : std::string_view{};
  }
};

// PRICE_FILTER: the tick and the price range.
struct PriceFilterRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& /*s*/) noexcept {
    if (!c.price) {
      return {};
    }
    const model::InstrumentCommon& i = *c.instrument;
    const std::int64_t tick = i.price_increment.raw();
    if (tick > 0 && c.price->raw() % tick != 0) {
      return reason::kPriceInvalidTick;
    }
    if ((i.min_price && *c.price < *i.min_price) || (i.max_price && *c.price > *i.max_price)) {
      return reason::kPriceOutOfRange;
    }
    return {};
  }
};

// LOT_SIZE: the step and the quantity range.
struct LotSizeRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& /*s*/) noexcept {
    const model::InstrumentCommon& i = *c.instrument;
    const std::uint64_t step = i.size_increment.raw();
    if (step > 0 && c.quantity.raw() % step != 0) {
      return reason::kQuantityInvalidStep;
    }
    if ((i.min_quantity && c.quantity.raw() < i.min_quantity->raw()) ||
        (i.max_quantity && c.quantity.raw() > i.max_quantity->raw())) {
      return reason::kQuantityOutOfRange;
    }
    return {};
  }
};

// MIN_NOTIONAL; Binance exempts reduce-only orders.
struct MinNotionalRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& /*s*/) noexcept {
    const std::optional<model::Price> px = detail::order_price(c);
    const std::optional<model::Money>& min = c.instrument->min_notional;
    if (!min || !px || c.reduce_only || !detail::new_order(c)) {
      return {};
    }
    const std::optional<model::Money> n =
        detail::notional(*c.instrument, c.quantity.raw(), c.quantity.precision(), *px);
    return n && n->currency() == min->currency() && n->raw() < min->raw()
               ? reason::kNotionalBelowMin
               : std::string_view{};
  }
};

// [risk] max_order_notional and the instrument's max_notional.
struct MaxOrderNotionalRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    const std::optional<model::Price> px = detail::order_price(c);
    if (!px) {
      return {};
    }
    const std::optional<model::Money> n =
        detail::notional(*c.instrument, c.quantity.raw(), c.quantity.precision(), *px);
    return detail::exceeds(n, s.config->max_order_notional) ||
                   detail::exceeds(n, c.instrument->max_notional)
               ? reason::kNotionalExceedsMax
               : std::string_view{};
  }
};

// A limit price further than price_band_bps from the reference.
struct PriceBandRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    const std::uint32_t band = s.config->price_band_bps;
    if (band == 0 || !c.price || !c.reference || c.reference->raw() <= 0) {
      return {};
    }
    const core::i128 gap = core::i128{c.price->raw()} - c.reference->raw();
    const core::i128 distance = gap < 0 ? -gap : gap;
    return distance * 10'000 > core::i128{c.reference->raw()} * band ? reason::kPriceOutsideBand
                                                                     : std::string_view{};
  }
};

struct MaxOpenOrdersRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    const std::uint32_t max = s.config->max_open_orders;
    return max > 0 && detail::new_order(c) && c.open.orders >= max ? reason::kOpenOrdersExceeded
                                                                   : std::string_view{};
  }
};

// One-way mode: a reduce-only order must reduce the venue position.
struct PositionModeRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& /*s*/) noexcept {
    return c.reduce_only && detail::new_order(c) && c.kind != CommandKind::Reduce
               ? reason::kReduceOnlyInvalid
               : std::string_view{};
  }
};

// The order's initial margin must fit in what the account has left. Orders that only reduce
// free margin and always pass.
struct MarginRule {
  [[nodiscard]] static constexpr std::string_view check(const OrderCheck& c,
                                                        const GateState& s) noexcept {
    if (!s.config->check_margin || !c.margin_known || c.kind != CommandKind::Open) {
      return {};
    }
    return c.required_raw > c.available_raw ? reason::kMarginInsufficient : std::string_view{};
  }
};

using GateA =
    std::tuple<TradingStateRule, InstrumentWhitelistRule, InstrumentStatusRule, IntentNotionalRule>;
using GateB = std::tuple<TradingStateRule, PriceFilterRule, LotSizeRule, MinNotionalRule,
                         MaxOrderNotionalRule, PriceBandRule, MaxOpenOrdersRule, PositionModeRule,
                         MarginRule>;
// A modify changes price and quantity of an order that already passed both gates.
using ModifyGate =
    std::tuple<TradingStateRule, PriceFilterRule, LotSizeRule, MaxOrderNotionalRule, PriceBandRule>;

template <typename Gate>
[[nodiscard]] constexpr std::string_view run_gate(const OrderCheck& c,
                                                  const GateState& s) noexcept {
  std::string_view denied;
  std::apply(
      [&](const auto&... rule) {
        static_cast<void>(((denied = rule.check(c, s), denied.empty()) && ...));
      },
      Gate{});
  return denied;
}

struct RiskStats {
  std::uint64_t checked = 0;
  std::uint64_t denied_a = 0;
  std::uint64_t denied_b = 0;
  std::uint64_t modify_rejected = 0;
  std::uint64_t kill_switches = 0;
  std::uint64_t state_changes = 0;
};

// The gates' state: configuration, TradingState, rate limit, instrument statuses, whitelists
// and the loss monitor.
class RiskEngine {
public:
  RiskEngine(const RiskConfig& config, std::uint32_t instruments, std::uint32_t strategies)
      : config_{config}, state_{config.initial_state}, limiter_{windows(config)},
        monitor_{limits(config)}, instruments_{instruments}, trading_{instruments},
        allowed_{std::size_t{instruments} * strategies}, restricted_{strategies} {
    for (std::uint32_t i = 0; i < instruments; ++i) {
      static_cast<void>(trading_.push_back(1));
    }
    for (std::size_t i = 0; i < allowed_.capacity(); ++i) {
      static_cast<void>(allowed_.push_back(0));
    }
    for (std::uint32_t i = 0; i < strategies; ++i) {
      static_cast<void>(restricted_.push_back(0));
    }
  }

  [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }
  [[nodiscard]] model::TradingState trading_state() const noexcept { return state_.state(); }
  [[nodiscard]] const TradingStateMachine& state_machine() const noexcept { return state_; }
  [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }
  [[nodiscard]] LossMonitor& monitor() noexcept { return monitor_; }
  [[nodiscard]] RateLimiter& limiter() noexcept { return limiter_; }

  // Returns whether the effective state changed.
  bool apply(TradingTrigger trigger) noexcept {
    const bool changed = state_.apply(trigger);
    stats_.state_changes += changed ? 1U : 0U;
    return changed;
  }

  void note_kill_switch() noexcept { ++stats_.kill_switches; }

  // Restricts strategy `s` to the instruments later allowed with allow().
  void restrict(std::uint16_t s) noexcept {
    if (s < restricted_.size()) {
      restricted_[s] = 1;
    }
  }
  void allow(std::uint16_t s, std::uint32_t slot) noexcept {
    if (s < restricted_.size() && slot < instruments_) {
      allowed_[std::size_t{s} * instruments_ + slot] = 1;
    }
  }

  void on_status(std::uint32_t slot, const model::InstrumentStatus& status) noexcept {
    if (slot >= trading_.size()) {
      return;
    }
    bool trading = true;
    switch (status.action) {
    case model::MarketStatusAction::Halt:
    case model::MarketStatusAction::Pause:
    case model::MarketStatusAction::Suspend:
    case model::MarketStatusAction::Close:
    case model::MarketStatusAction::PostClose:
    case model::MarketStatusAction::NotAvailableForTrading:
      trading = false;
      break;
    case model::MarketStatusAction::None:
    case model::MarketStatusAction::PreOpen:
    case model::MarketStatusAction::PreCross:
    case model::MarketStatusAction::Quoting:
    case model::MarketStatusAction::Cross:
    case model::MarketStatusAction::Rotation:
    case model::MarketStatusAction::NewPriceIndication:
    case model::MarketStatusAction::Trading:
    case model::MarketStatusAction::PreClose:
    case model::MarketStatusAction::ShortSellRestrictionChange:
      break;
    }
    if (status.is_trading) {
      trading = *status.is_trading;
    }
    trading_[slot] = trading ? 1 : 0;
  }

  [[nodiscard]] bool instrument_trading(std::uint32_t slot) const noexcept {
    return slot >= trading_.size() || trading_[slot] != 0;
  }

  // Gate A then Gate B, then the rate limit; empty when the order may go.
  [[nodiscard]] std::string_view check_order(const OrderCheck& c) noexcept {
    ++stats_.checked;
    const GateState s = gate_state(c);
    std::string_view denied = run_gate<GateA>(c, s);
    if (!denied.empty()) {
      ++stats_.denied_a;
      return denied;
    }
    denied = run_gate<GateB>(c, s);
    if (denied.empty() && !limiter_.try_acquire(c.now)) {
      denied = reason::kRateLimitExceeded;
    }
    stats_.denied_b += denied.empty() ? 0U : 1U;
    return denied;
  }

  // A parent order of an execution algorithm: Gate A only; its children pass check_child.
  [[nodiscard]] std::string_view check_parent(const OrderCheck& c) noexcept {
    ++stats_.checked;
    const std::string_view denied = run_gate<GateA>(c, gate_state(c));
    stats_.denied_a += denied.empty() ? 0U : 1U;
    return denied;
  }

  // A child order of an execution algorithm (its parent passed Gate A): Gate B, then the rate
  // limit.
  [[nodiscard]] std::string_view check_child(const OrderCheck& c) noexcept {
    ++stats_.checked;
    std::string_view denied = run_gate<GateB>(c, gate_state(c));
    if (denied.empty() && !limiter_.try_acquire(c.now)) {
      denied = reason::kRateLimitExceeded;
    }
    stats_.denied_b += denied.empty() ? 0U : 1U;
    return denied;
  }

  [[nodiscard]] std::string_view check_modify(const OrderCheck& c) noexcept {
    ++stats_.checked;
    std::string_view denied = run_gate<ModifyGate>(c, gate_state(c));
    if (denied.empty() && !limiter_.try_acquire(c.now)) {
      denied = reason::kRateLimitExceeded;
    }
    stats_.modify_rejected += denied.empty() ? 0U : 1U;
    return denied;
  }

private:
  [[nodiscard]] static MonitorLimits limits(const RiskConfig& c) noexcept {
    const auto raw = [](const std::optional<model::Money>& m) -> std::optional<std::int64_t> {
      if (!m) {
        return std::nullopt;
      }
      return static_cast<std::int64_t>(core::magnitude(m->raw()));
    };
    return MonitorLimits{raw(c.daily_loss_limit), raw(c.daily_loss_halt), raw(c.max_drawdown),
                         c.margin_ratio_bps};
  }

  [[nodiscard]] static std::array<RateWindow, 2> windows(const RiskConfig& c) noexcept {
    return {RateWindow{c.orders_per_10s > 0 ? 10'000'000'000ULL : 0, c.orders_per_10s},
            RateWindow{c.orders_per_minute > 0 ? 60'000'000'000ULL : 0, c.orders_per_minute}};
  }

  [[nodiscard]] GateState gate_state(const OrderCheck& c) const noexcept {
    GateState s;
    s.config = &config_;
    s.trading_state = state_.state();
    s.instrument_allowed =
        c.strategy >= restricted_.size() || restricted_[c.strategy] == 0 ||
        (c.slot < instruments_ && allowed_[std::size_t{c.strategy} * instruments_ + c.slot] != 0);
    s.instrument_trading = instrument_trading(c.slot);
    return s;
  }

  RiskConfig config_;
  TradingStateMachine state_;
  RateLimiter limiter_;
  LossMonitor monitor_;
  std::uint32_t instruments_;
  core::FixedVector<std::uint8_t> trading_;    // by slot
  core::FixedVector<std::uint8_t> allowed_;    // strategy x slot
  core::FixedVector<std::uint8_t> restricted_; // by strategy
  RiskStats stats_;
};

} // namespace jarvis::risk
