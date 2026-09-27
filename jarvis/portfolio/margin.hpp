#pragma once

#include <concepts>
#include <cstdint>
#include <variant>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"

// Margin models (docs/architecture.md section 11.2): the initial margin a position or an order
// ties up and the maintenance margin below which the venue liquidates. Both round up onto the
// currency's grid, so the kernel never assumes more free margin than the venue grants.

namespace jarvis::portfolio {

template <typename M>
concept MarginModel = requires(const M& m, const model::InstrumentCommon& instrument,
                               model::Quantity quantity, model::Price price, model::Money& out) {
  { m.initial(instrument, quantity, price, out) } -> std::same_as<core::Status>;
  { m.maintenance(instrument, quantity, price, out) } -> std::same_as<core::Status>;
  // The initial margin as a fraction of notional (10^9 scale), for aggregate notionals.
  { m.initial_rate(instrument) } -> std::same_as<std::uint64_t>;
};

namespace detail {

// notional x fraction (10^9 scale), rounded up onto the notional currency's grid.
[[nodiscard]] constexpr core::Status notional_times(const model::InstrumentCommon& instrument,
                                                    model::Quantity quantity, model::Price price,
                                                    std::uint64_t fraction_raw,
                                                    std::uint64_t divisor,
                                                    model::Money& out) noexcept {
  model::Money notional;
  core::Status s = model::notional_value(instrument, quantity, price, notional);
  if (!core::ok(s)) {
    return s;
  }
  std::uint64_t magnitude = 0;
  s = core::mul_div_u64_up(core::magnitude(notional.raw()), fraction_raw, 1, divisor, magnitude);
  if (!core::ok(s)) {
    return s;
  }
  const std::uint64_t unit = core::kPow10[model::kFixedPrecision - notional.currency().precision()];
  const std::uint64_t down = magnitude / unit * unit;
  const std::uint64_t up = down == magnitude ? down : down + unit;
  if (up > static_cast<std::uint64_t>(model::kMoneyRawMax)) {
    return core::Status::Overflow;
  }
  return model::Money::from_raw(static_cast<std::int64_t>(up), notional.currency(), out);
}

inline constexpr std::uint64_t kScale = 1'000'000'000ULL;

} // namespace detail

// Fixed fractions from the instrument definition (nautilus StandardMarginModel):
// initial = notional x margin_init, maintenance = notional x margin_maint.
class StandardMargin {
public:
  [[nodiscard]] static constexpr std::uint64_t
  initial_rate(const model::InstrumentCommon& instrument) noexcept {
    return core::magnitude(instrument.margin_init.raw());
  }
  [[nodiscard]] static constexpr core::Status initial(const model::InstrumentCommon& instrument,
                                                      model::Quantity quantity, model::Price price,
                                                      model::Money& out) noexcept {
    return detail::notional_times(instrument, quantity, price,
                                  core::magnitude(instrument.margin_init.raw()), detail::kScale,
                                  out);
  }
  [[nodiscard]] static constexpr core::Status maintenance(const model::InstrumentCommon& instrument,
                                                          model::Quantity quantity,
                                                          model::Price price,
                                                          model::Money& out) noexcept {
    return detail::notional_times(instrument, quantity, price,
                                  core::magnitude(instrument.margin_maint.raw()), detail::kScale,
                                  out);
  }
};

// Binance USD-M: initial = notional / leverage, maintenance = notional x margin_maint.
class LeveragedMargin {
public:
  explicit constexpr LeveragedMargin(std::uint32_t leverage) noexcept
      : leverage_{leverage == 0 ? 1U : leverage} {}

  [[nodiscard]] constexpr std::uint32_t leverage() const noexcept { return leverage_; }

  // 1 / leverage, rounded up.
  [[nodiscard]] constexpr std::uint64_t
  initial_rate(const model::InstrumentCommon& /*instrument*/) const noexcept {
    return (detail::kScale + leverage_ - 1) / leverage_;
  }

  [[nodiscard]] constexpr core::Status initial(const model::InstrumentCommon& instrument,
                                               model::Quantity quantity, model::Price price,
                                               model::Money& out) const noexcept {
    return detail::notional_times(instrument, quantity, price, detail::kScale,
                                  detail::kScale * leverage_, out);
  }
  [[nodiscard]] static constexpr core::Status maintenance(const model::InstrumentCommon& instrument,
                                                          model::Quantity quantity,
                                                          model::Price price,
                                                          model::Money& out) noexcept {
    return StandardMargin::maintenance(instrument, quantity, price, out);
  }

private:
  std::uint32_t leverage_;
};

static_assert(MarginModel<StandardMargin>);
static_assert(MarginModel<LeveragedMargin>);

using Margin = std::variant<StandardMargin, LeveragedMargin>;

} // namespace jarvis::portfolio
