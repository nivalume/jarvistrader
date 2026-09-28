#pragma once

#include <cstdint>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"

// A netting position (docs/architecture.md sections 8.5 and 11.2) in integers:
//
//   signed quantity   raw at 10^9, positive long and negative short;
//   open notional     sum of price.raw x quantity.raw over the open quantity (10^18 scale), so
//                     the average open price is exact and a full close realizes exactly the
//                     difference between the exit and entry notionals;
//   PnL               raw at 10^9 in the settlement currency, for linear contracts:
//                     (exit - entry) x quantity x multiplier, sign by side.
//
// apply() takes at most what closes the position; the caller splits a fill that flips it
// (closing part, then opening part) so each part produces its own position event.

namespace jarvis::portfolio {

namespace position_detail {

inline constexpr std::uint64_t kScale = 1'000'000'000ULL;

// |x| * m / 10^18 through 192 bits; Overflow when the result exceeds int64.
[[nodiscard]] constexpr core::Status scale_down(core::u128 x, std::uint64_t m,
                                                std::int64_t& out) noexcept {
  const core::U192 product = core::mul_u128_u64(x, m);
  const core::U192 q = core::div_u192_u64(core::div_u192_u64(product, kScale), kScale);
  if (q.hi != 0 || q.mid != 0 || q.lo > static_cast<std::uint64_t>(INT64_MAX)) {
    return core::Status::Overflow;
  }
  out = static_cast<std::int64_t>(q.lo);
  return core::Status::Ok;
}

// A signed 10^18-scale amount times the multiplier, back to 10^9 (truncated toward zero).
[[nodiscard]] constexpr core::Status signed_scale_down(core::u128 gain, core::u128 loss,
                                                       std::uint64_t multiplier_raw,
                                                       std::int64_t& out) noexcept {
  const bool negative = loss > gain;
  std::int64_t magnitude = 0;
  const core::Status s =
      scale_down(negative ? loss - gain : gain - loss, multiplier_raw, magnitude);
  out = negative ? -magnitude : magnitude;
  return s;
}

} // namespace position_detail

class NettingPosition {
public:
  // The quantity a fill on `side` can close (0 when it would add to the position).
  [[nodiscard]] constexpr std::uint64_t closable(model::OrderSide side) const noexcept {
    if (signed_raw_ == 0 || (signed_raw_ > 0) == (side == model::OrderSide::Buy)) {
      return 0;
    }
    return core::magnitude(signed_raw_);
  }

  // Applies a fill of `quantity` at `price`, which must not exceed closable() when it reduces
  // the position. `realized_raw` receives the PnL this fill realized.
  [[nodiscard]] constexpr core::Status apply(model::OrderSide side, model::Quantity quantity,
                                             model::Price price, std::uint64_t multiplier_raw,
                                             const model::ClientOrderId& order, core::UnixNanos ts,
                                             std::int64_t& realized_raw) noexcept {
    realized_raw = 0;
    const std::uint64_t q = quantity.raw();
    const std::uint64_t px = core::magnitude(price.raw());
    const std::uint64_t held = core::magnitude(signed_raw_);
    const std::uint64_t closing = closable(side);
    if (q == 0 || price.raw() <= 0) {
      return core::Status::InvalidArgument;
    }
    if (closing == 0) {
      if (q > static_cast<std::uint64_t>(INT64_MAX) - held) {
        return core::Status::Overflow;
      }
      if (signed_raw_ == 0) {
        open(side, quantity.precision(), order, ts);
      }
      open_notional_ += static_cast<core::u128>(px) * q;
      signed_raw_ += side == model::OrderSide::Buy ? static_cast<std::int64_t>(q)
                                                   : -static_cast<std::int64_t>(q);
      // Reductions remove basis in proportion, so the average only moves on increases.
      avg_open_raw_ = static_cast<std::uint64_t>(open_notional_ / core::magnitude(signed_raw_));
    } else {
      if (q > closing) {
        return core::Status::InvalidArgument; // the caller splits flips
      }
      const core::U192 scaled = core::div_u192_u64(core::mul_u128_u64(open_notional_, q), held);
      const core::u128 basis =
          q == held ? open_notional_ : (static_cast<core::u128>(scaled.mid) << 64U) | scaled.lo;
      const core::u128 exit = static_cast<core::u128>(px) * q;
      const bool long_side = signed_raw_ > 0;
      const core::Status s = position_detail::signed_scale_down(
          long_side ? exit : basis, long_side ? basis : exit, multiplier_raw, realized_raw);
      if (!core::ok(s)) {
        return s;
      }
      open_notional_ -= basis;
      close_notional_ += exit;
      close_raw_ += q;
      signed_raw_ += long_side ? -static_cast<std::int64_t>(q) : static_cast<std::int64_t>(q);
      if (signed_raw_ == 0) {
        open_notional_ = 0;
      }
      realized_raw_ += realized_raw;
      total_realized_raw_ += realized_raw;
    }
    const std::uint64_t now_held = core::magnitude(signed_raw_);
    peak_raw_ = now_held > peak_raw_ ? now_held : peak_raw_;
    last_px_ = price;
    last_qty_ = quantity;
    return core::Status::Ok;
  }

  // Replaces the position with `signed_raw` held at `avg_px` (reconciliation sets the venue's
  // position, docs/architecture.md section 15.2). The totals since the node started stay; the
  // per-position figures restart as for a newly opened position.
  constexpr void reset(std::int64_t signed_raw, std::uint8_t precision, model::Price avg_px,
                       core::UnixNanos ts) noexcept {
    open(signed_raw < 0 ? model::OrderSide::Sell : model::OrderSide::Buy, precision,
         model::ClientOrderId{}, ts);
    signed_raw_ = signed_raw;
    const std::uint64_t held = core::magnitude(signed_raw);
    avg_open_raw_ = held == 0 ? 0 : core::magnitude(avg_px.raw());
    open_notional_ = static_cast<core::u128>(avg_open_raw_) * held;
    peak_raw_ = held;
  }

  // Commission paid (positive) or a rebate (negative), in the settlement currency.
  constexpr void add_commission(std::int64_t raw) noexcept {
    commission_raw_ += raw;
    total_commission_raw_ += raw;
  }
  // Funding received (positive) or paid (negative), in the settlement currency.
  constexpr void add_funding(std::int64_t raw) noexcept {
    funding_raw_ += raw;
    total_funding_raw_ += raw;
  }

  // PnL of the open quantity at `mark` (10^9 raw, settlement currency).
  [[nodiscard]] constexpr core::Status unrealized(model::Price mark, std::uint64_t multiplier_raw,
                                                  std::int64_t& out) const noexcept {
    out = 0;
    if (signed_raw_ == 0) {
      return core::Status::Ok;
    }
    const core::u128 value =
        static_cast<core::u128>(core::magnitude(mark.raw())) * core::magnitude(signed_raw_);
    const bool long_side = signed_raw_ > 0;
    return position_detail::signed_scale_down(long_side ? value : open_notional_,
                                              long_side ? open_notional_ : value, multiplier_raw,
                                              out);
  }

  [[nodiscard]] constexpr std::int64_t signed_raw() const noexcept { return signed_raw_; }
  [[nodiscard]] constexpr std::uint64_t quantity_raw() const noexcept {
    return core::magnitude(signed_raw_);
  }
  [[nodiscard]] constexpr std::uint8_t size_precision() const noexcept { return size_precision_; }
  [[nodiscard]] constexpr bool is_open() const noexcept { return signed_raw_ != 0; }
  [[nodiscard]] constexpr model::PositionSide side() const noexcept {
    if (signed_raw_ == 0) {
      return model::PositionSide::Flat;
    }
    return signed_raw_ > 0 ? model::PositionSide::Long : model::PositionSide::Short;
  }
  [[nodiscard]] constexpr model::OrderSide entry() const noexcept { return entry_; }
  [[nodiscard]] constexpr core::u128 open_notional() const noexcept { return open_notional_; }
  [[nodiscard]] constexpr std::uint64_t peak_raw() const noexcept { return peak_raw_; }
  [[nodiscard]] constexpr std::int64_t realized_raw() const noexcept { return realized_raw_; }
  [[nodiscard]] constexpr std::int64_t commission_raw() const noexcept { return commission_raw_; }
  [[nodiscard]] constexpr std::int64_t funding_raw() const noexcept { return funding_raw_; }
  // Since the node started, across every opening of this position.
  [[nodiscard]] constexpr std::int64_t total_realized_raw() const noexcept {
    return total_realized_raw_;
  }
  [[nodiscard]] constexpr std::int64_t total_commission_raw() const noexcept {
    return total_commission_raw_;
  }
  [[nodiscard]] constexpr std::int64_t total_funding_raw() const noexcept {
    return total_funding_raw_;
  }
  [[nodiscard]] constexpr const model::ClientOrderId& opening_order_id() const noexcept {
    return opening_order_id_;
  }
  [[nodiscard]] constexpr core::UnixNanos ts_opened() const noexcept { return ts_opened_; }
  [[nodiscard]] constexpr model::Price last_px() const noexcept { return last_px_; }
  [[nodiscard]] constexpr model::Quantity last_qty() const noexcept { return last_qty_; }

  // Average price of the opening fills at full fixed-point precision (9 decimals), kept after
  // the position closes; false before it first opened.
  [[nodiscard]] constexpr bool avg_px_open(model::Price& out) const noexcept {
    return average(avg_open_raw_, 1, out);
  }
  // Average price of the closing fills since the position opened; false before any.
  [[nodiscard]] constexpr bool avg_px_close(model::Price& out) const noexcept {
    return average(close_notional_, close_raw_, out);
  }

private:
  constexpr void open(model::OrderSide side, std::uint8_t precision,
                      const model::ClientOrderId& order, core::UnixNanos ts) noexcept {
    entry_ = side;
    size_precision_ = precision;
    opening_order_id_ = order;
    ts_opened_ = ts;
    open_notional_ = 0;
    close_notional_ = 0;
    close_raw_ = 0;
    avg_open_raw_ = 0;
    peak_raw_ = 0;
    realized_raw_ = 0;
    commission_raw_ = 0;
    funding_raw_ = 0;
  }

  [[nodiscard]] static constexpr bool average(core::u128 notional, std::uint64_t quantity,
                                              model::Price& out) noexcept {
    if (quantity == 0 || notional == 0) {
      return false;
    }
    const core::u128 avg = notional / quantity;
    if (avg > static_cast<core::u128>(model::kPriceRawMax)) {
      return false;
    }
    return core::ok(
        model::Price::from_raw(static_cast<std::int64_t>(avg), model::kFixedPrecision, out));
  }

  std::int64_t signed_raw_ = 0;
  std::uint8_t size_precision_ = 0;
  model::OrderSide entry_ = model::OrderSide::Buy;
  core::u128 open_notional_ = 0;
  core::u128 close_notional_ = 0;
  std::uint64_t close_raw_ = 0;
  std::uint64_t avg_open_raw_ = 0;
  std::uint64_t peak_raw_ = 0;
  std::int64_t realized_raw_ = 0;
  std::int64_t commission_raw_ = 0;
  std::int64_t funding_raw_ = 0;
  std::int64_t total_realized_raw_ = 0;
  std::int64_t total_commission_raw_ = 0;
  std::int64_t total_funding_raw_ = 0;
  model::ClientOrderId opening_order_id_;
  core::UnixNanos ts_opened_;
  model::Price last_px_;
  model::Quantity last_qty_;
};

} // namespace jarvis::portfolio
