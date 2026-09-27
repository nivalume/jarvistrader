#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <string_view>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"

// Trading fees and funding (docs/architecture.md section 11.1). Both are computed exactly in
// integers from the instrument's notional value and rounded onto the currency's grid against
// the account: fees and funding paid round up, rebates and funding received round down. A
// backtest therefore never books a cost smaller than the venue would charge.

namespace jarvis::cost {

namespace detail {

inline constexpr std::uint64_t kScale = 1'000'000'000ULL; // 10^9, the fixed-point scale

// Rounds a raw magnitude (10^9 scale) onto the grid of `precision` decimals. Rounding up is
// exact when the magnitude itself was rounded up (core::mul_div_u64_up).
[[nodiscard]] constexpr std::uint64_t to_grid(std::uint64_t magnitude, std::uint8_t precision,
                                              bool up) noexcept {
  const std::uint64_t unit = core::kPow10[model::kFixedPrecision - precision];
  const std::uint64_t down = magnitude / unit * unit;
  return up && down != magnitude ? down + unit : down;
}

// A signed amount from a magnitude and a sign, onto the currency's grid.
[[nodiscard]] constexpr core::Status signed_money(std::uint64_t magnitude, bool negative, bool up,
                                                  const model::Currency& currency,
                                                  model::Money& out) noexcept {
  const std::uint64_t m = to_grid(magnitude, currency.precision(), up);
  if (m > static_cast<std::uint64_t>(model::kMoneyRawMax)) {
    return core::Status::Overflow;
  }
  const auto raw = static_cast<std::int64_t>(m);
  return model::Money::from_raw(negative ? -raw : raw, currency, out);
}

} // namespace detail

// Computes the commission of one fill. Positive = paid, negative = a rebate.
template <typename F>
concept FeeModel =
    requires(const F& f, const model::InstrumentCommon& instrument, model::LiquiditySide side,
             model::Quantity quantity, model::Price price, model::Money& out) {
      { f.commission(instrument, side, quantity, price, out) } -> std::same_as<core::Status>;
    };

// Maker and taker rates (fractions of notional: 0.0002 is 2 bps; negative maker rates are
// rebates) and a discount on positive fees (Binance: 10% on USD-M futures paid in BNB).
class MakerTakerFees {
public:
  constexpr MakerTakerFees() noexcept = default;

  [[nodiscard]] static constexpr core::Status create(model::Decimal maker, model::Decimal taker,
                                                     model::Decimal discount,
                                                     MakerTakerFees& out) noexcept {
    const auto one = static_cast<std::int64_t>(detail::kScale);
    if (discount.raw() < 0 || discount.raw() >= one || maker.raw() <= -one || maker.raw() >= one ||
        taker.raw() < 0 || taker.raw() >= one) {
      return core::Status::InvalidArgument;
    }
    out.maker_ = maker;
    out.taker_ = taker;
    out.discount_ = discount;
    return core::Status::Ok;
  }

  // Built-in schedules (Binance standard rates, 2026):
  //   binance_usdm_vip0       maker 0.0200%  taker 0.0500%
  //   binance_usdm_vip0_bnb   the same, 10% off when paid in BNB
  //   binance_spot_vip0       maker 0.1000%  taker 0.1000%
  //   binance_spot_vip0_bnb   the same, 25% off when paid in BNB
  //   zero                    no fees
  [[nodiscard]] static constexpr core::Status schedule(std::string_view name,
                                                       MakerTakerFees& out) noexcept {
    struct Entry {
      std::string_view name;
      std::int64_t maker_raw;
      std::int64_t taker_raw;
      std::int64_t discount_raw;
    };
    constexpr std::array<Entry, 5> kSchedules = {{
        {"binance_usdm_vip0", 200'000, 500'000, 0},
        {"binance_usdm_vip0_bnb", 200'000, 500'000, 100'000'000},
        {"binance_spot_vip0", 1'000'000, 1'000'000, 0},
        {"binance_spot_vip0_bnb", 1'000'000, 1'000'000, 250'000'000},
        {"zero", 0, 0, 0},
    }};
    for (const Entry& e : kSchedules) {
      if (e.name == name) {
        model::Decimal maker;
        model::Decimal taker;
        model::Decimal discount;
        static_cast<void>(model::Decimal::from_raw(e.maker_raw, 4, maker));
        static_cast<void>(model::Decimal::from_raw(e.taker_raw, 4, taker));
        static_cast<void>(model::Decimal::from_raw(e.discount_raw, 2, discount));
        return create(maker, taker, discount, out);
      }
    }
    return core::Status::NotFound;
  }

  [[nodiscard]] constexpr model::Decimal maker() const noexcept { return maker_; }
  [[nodiscard]] constexpr model::Decimal taker() const noexcept { return taker_; }
  [[nodiscard]] constexpr model::Decimal discount() const noexcept { return discount_; }

  // notional x rate x (1 - discount) in the notional's currency. A fill without a liquidity side
  // pays the taker rate.
  [[nodiscard]] constexpr core::Status commission(const model::InstrumentCommon& instrument,
                                                  model::LiquiditySide side,
                                                  model::Quantity quantity, model::Price price,
                                                  model::Money& out) const noexcept {
    model::Money notional;
    core::Status s = model::notional_value(instrument, quantity, price, notional);
    if (!core::ok(s)) {
      return s;
    }
    const model::Decimal rate = side == model::LiquiditySide::Maker ? maker_ : taker_;
    const bool rebate = rate.is_negative();
    const std::uint64_t keep =
        rebate ? detail::kScale : detail::kScale - static_cast<std::uint64_t>(discount_.raw());
    std::uint64_t magnitude = 0;
    const auto divide = rebate ? core::mul_div_u64 : core::mul_div_u64_up;
    s = divide(core::magnitude(notional.raw()), core::magnitude(rate.raw()), keep,
               detail::kScale * detail::kScale, magnitude);
    if (!core::ok(s)) {
      return s;
    }
    return detail::signed_money(magnitude, rebate, !rebate, notional.currency(), out);
  }

private:
  model::Decimal maker_;
  model::Decimal taker_;
  model::Decimal discount_;
};

static_assert(FeeModel<MakerTakerFees>);

// Funding exchanged by a position at a funding settlement (Binance USD-M perpetuals): the
// position's notional at the mark price times the rate; longs pay a positive rate to shorts.
// `out` is positive when the position receives and negative when it pays, in the notional's
// currency (quote for linear contracts, base for inverse ones).
[[nodiscard]] constexpr core::Status funding_payment(const model::InstrumentCommon& instrument,
                                                     model::PositionSide side,
                                                     model::Quantity quantity, model::Price mark,
                                                     model::Decimal rate,
                                                     model::Money& out) noexcept {
  if (side == model::PositionSide::Flat || quantity.is_zero() || rate.is_zero()) {
    const model::Currency& currency = instrument.is_inverse && instrument.base_currency
                                          ? *instrument.base_currency
                                          : instrument.quote_currency;
    return model::Money::from_raw(0, currency, out);
  }
  model::Money notional;
  core::Status s = model::notional_value(instrument, quantity, mark, notional);
  if (!core::ok(s)) {
    return s;
  }
  const bool pays = (side == model::PositionSide::Long) == rate.is_positive();
  std::uint64_t magnitude = 0;
  const auto divide = pays ? core::mul_div_u64_up : core::mul_div_u64;
  s = divide(core::magnitude(notional.raw()), core::magnitude(rate.raw()), 1, detail::kScale,
             magnitude);
  if (!core::ok(s)) {
    return s;
  }
  return detail::signed_money(magnitude, pays, pays, notional.currency(), out);
}

} // namespace jarvis::cost
