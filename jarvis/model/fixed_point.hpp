#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/decimal.hpp"

namespace jarvis::model {

inline constexpr std::int64_t kPriceRawMax = 9'223'372'036'000'000'000;
inline constexpr std::int64_t kPriceRawMin = -kPriceRawMax;
inline constexpr std::uint64_t kQuantityRawMax = 18'446'744'073'000'000'000ULL;
inline constexpr std::int64_t kPriceUndef = INT64_MAX;
inline constexpr std::int64_t kPriceError = INT64_MIN;
inline constexpr std::uint64_t kQuantityUndef = UINT64_MAX;

namespace detail {

[[nodiscard]] constexpr bool on_grid(core::u128 magnitude, std::uint8_t precision) noexcept {
  return magnitude % core::kPow10[kFixedPrecision - precision] == 0;
}

} // namespace detail

// Signed fixed-point value: raw int64 at 10^9 scale plus a display precision. Used for Price and
// for Decimal (ratios, rates, offsets). Equality and ordering compare raw values only, so
// Price(1.23, 2) == Price(1.230, 3), exactly like nautilus.
template <typename Tag> class SignedFixed {
public:
  constexpr SignedFixed() noexcept = default;

  // raw must lie in [kPriceRawMin, kPriceRawMax] and be a multiple of 10^(9 - precision).
  [[nodiscard]] static constexpr core::Status from_raw(std::int64_t raw, std::uint8_t precision,
                                                       SignedFixed& out) noexcept {
    if (precision > kFixedPrecision) {
      return core::Status::InvalidArgument;
    }
    if (raw < kPriceRawMin || raw > kPriceRawMax) {
      return core::Status::OutOfRange;
    }
    if (!detail::on_grid(core::magnitude(raw), precision)) {
      return core::Status::PrecisionLoss;
    }
    out = SignedFixed{raw, precision};
    return core::Status::Ok;
  }

  // Precision is inferred from the number of fraction digits.
  [[nodiscard]] static constexpr core::Status parse(std::string_view text,
                                                    SignedFixed& out) noexcept {
    detail::ParsedDecimal d;
    core::Status s = detail::parse_decimal_text(text, d);
    if (!core::ok(s)) {
      return s;
    }
    if (detail::implied_precision(d) > kFixedPrecision) {
      return core::Status::PrecisionLoss;
    }
    return from_parsed(d, static_cast<std::uint8_t>(detail::implied_precision(d)), out);
  }

  // Parses at a fixed precision; extra fraction digits must be zeros.
  [[nodiscard]] static constexpr core::Status parse(std::string_view text, std::uint8_t precision,
                                                    SignedFixed& out) noexcept {
    detail::ParsedDecimal d;
    const core::Status s = detail::parse_decimal_text(text, d);
    if (!core::ok(s)) {
      return s;
    }
    return from_parsed(d, precision, out);
  }

  // value = mantissa * 10^exponent (the SBE decimal encoding), expressed at `precision`.
  [[nodiscard]] static constexpr core::Status from_mantissa_exponent(std::int64_t mantissa,
                                                                     std::int8_t exponent,
                                                                     std::uint8_t precision,
                                                                     SignedFixed& out) noexcept {
    detail::ParsedDecimal d;
    d.negative = mantissa < 0;
    d.mantissa = core::magnitude(mantissa);
    if (exponent < 0) {
      d.scale = static_cast<std::uint32_t>(-exponent);
    } else {
      core::u128 factor = 0;
      if (!detail::pow10_u128(static_cast<std::uint32_t>(exponent), factor)) {
        return core::Status::Overflow;
      }
      d.mantissa *= factor;
    }
    return from_parsed(d, precision, out);
  }

  [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr std::uint8_t precision() const noexcept { return precision_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
  [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_ > 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }

  [[nodiscard]] constexpr core::Status format(std::span<char> out,
                                              std::size_t& written) const noexcept {
    return detail::format_raw9(raw_, precision_, out, written);
  }

  // Sum and difference keep the larger precision; results outside the valid range overflow.
  [[nodiscard]] static constexpr core::Status add(SignedFixed a, SignedFixed b,
                                                  SignedFixed& out) noexcept {
    std::int64_t raw = 0;
    if (!core::checked_add(a.raw_, b.raw_, raw)) {
      return core::Status::Overflow;
    }
    return range_checked(raw, a.precision_ > b.precision_ ? a.precision_ : b.precision_, out);
  }
  [[nodiscard]] static constexpr core::Status sub(SignedFixed a, SignedFixed b,
                                                  SignedFixed& out) noexcept {
    std::int64_t raw = 0;
    if (!core::checked_sub(a.raw_, b.raw_, raw)) {
      return core::Status::Overflow;
    }
    return range_checked(raw, a.precision_ > b.precision_ ? a.precision_ : b.precision_, out);
  }

  friend constexpr bool operator==(SignedFixed a, SignedFixed b) noexcept {
    return a.raw_ == b.raw_;
  }
  friend constexpr std::strong_ordering operator<=>(SignedFixed a, SignedFixed b) noexcept {
    return a.raw_ <=> b.raw_;
  }

private:
  constexpr SignedFixed(std::int64_t raw, std::uint8_t precision) noexcept
      : raw_{raw}, precision_{precision} {}

  [[nodiscard]] static constexpr core::Status range_checked(std::int64_t raw,
                                                            std::uint8_t precision,
                                                            SignedFixed& out) noexcept {
    if (raw < kPriceRawMin || raw > kPriceRawMax) {
      return core::Status::Overflow;
    }
    out = SignedFixed{raw, precision};
    return core::Status::Ok;
  }

  [[nodiscard]] static constexpr core::Status
  from_parsed(const detail::ParsedDecimal& d, std::uint8_t precision, SignedFixed& out) noexcept {
    core::i128 raw9 = 0;
    const core::Status s = detail::to_raw9(d, precision, raw9);
    if (!core::ok(s)) {
      return s;
    }
    if (raw9 < kPriceRawMin || raw9 > kPriceRawMax) {
      return core::Status::OutOfRange;
    }
    out = SignedFixed{static_cast<std::int64_t>(raw9), precision};
    return core::Status::Ok;
  }

  std::int64_t raw_ = 0;
  std::uint8_t precision_ = 0;
};

struct PriceTag;
struct DecimalTag;

// A price (nautilus `Price`).
using Price = SignedFixed<PriceTag>;
// A dimensionless signed decimal: margins, fee and funding rates, offsets, returns. nautilus uses
// rust_decimal for these; jarvis keeps them on the same 10^9 integer grid.
using Decimal = SignedFixed<DecimalTag>;

// A non-negative quantity (nautilus `Quantity`): raw uint64 at 10^9 scale plus precision.
class Quantity {
public:
  constexpr Quantity() noexcept = default;

  [[nodiscard]] static constexpr core::Status from_raw(std::uint64_t raw, std::uint8_t precision,
                                                       Quantity& out) noexcept {
    if (precision > kFixedPrecision) {
      return core::Status::InvalidArgument;
    }
    if (raw > kQuantityRawMax) {
      return core::Status::OutOfRange;
    }
    if (!detail::on_grid(raw, precision)) {
      return core::Status::PrecisionLoss;
    }
    out = Quantity{raw, precision};
    return core::Status::Ok;
  }

  [[nodiscard]] static constexpr core::Status parse(std::string_view text, Quantity& out) noexcept {
    detail::ParsedDecimal d;
    const core::Status s = detail::parse_decimal_text(text, d);
    if (!core::ok(s)) {
      return s;
    }
    if (detail::implied_precision(d) > kFixedPrecision) {
      return core::Status::PrecisionLoss;
    }
    return from_parsed(d, static_cast<std::uint8_t>(detail::implied_precision(d)), out);
  }

  [[nodiscard]] static constexpr core::Status parse(std::string_view text, std::uint8_t precision,
                                                    Quantity& out) noexcept {
    detail::ParsedDecimal d;
    const core::Status s = detail::parse_decimal_text(text, d);
    if (!core::ok(s)) {
      return s;
    }
    return from_parsed(d, precision, out);
  }

  [[nodiscard]] static constexpr core::Status from_mantissa_exponent(std::int64_t mantissa,
                                                                     std::int8_t exponent,
                                                                     std::uint8_t precision,
                                                                     Quantity& out) noexcept {
    Price as_signed;
    const core::Status s = Price::from_mantissa_exponent(mantissa, exponent, precision, as_signed);
    if (!core::ok(s)) {
      return s;
    }
    if (as_signed.raw() < 0) {
      return core::Status::OutOfRange;
    }
    return from_raw(static_cast<std::uint64_t>(as_signed.raw()), precision, out);
  }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr std::uint8_t precision() const noexcept { return precision_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }

  [[nodiscard]] constexpr core::Status format(std::span<char> out,
                                              std::size_t& written) const noexcept {
    return detail::format_raw9(static_cast<core::i128>(raw_), precision_, out, written);
  }

  [[nodiscard]] static constexpr core::Status add(Quantity a, Quantity b, Quantity& out) noexcept {
    std::uint64_t raw = 0;
    if (!core::checked_add(a.raw_, b.raw_, raw) || raw > kQuantityRawMax) {
      return core::Status::Overflow;
    }
    out = Quantity{raw, a.precision_ > b.precision_ ? a.precision_ : b.precision_};
    return core::Status::Ok;
  }
  // Fails with OutOfRange when b exceeds a: quantities are never negative.
  [[nodiscard]] static constexpr core::Status sub(Quantity a, Quantity b, Quantity& out) noexcept {
    std::uint64_t raw = 0;
    if (!core::checked_sub(a.raw_, b.raw_, raw)) {
      return core::Status::OutOfRange;
    }
    out = Quantity{raw, a.precision_ > b.precision_ ? a.precision_ : b.precision_};
    return core::Status::Ok;
  }

  friend constexpr bool operator==(Quantity a, Quantity b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr std::strong_ordering operator<=>(Quantity a, Quantity b) noexcept {
    return a.raw_ <=> b.raw_;
  }

private:
  constexpr Quantity(std::uint64_t raw, std::uint8_t precision) noexcept
      : raw_{raw}, precision_{precision} {}

  [[nodiscard]] static constexpr core::Status
  from_parsed(const detail::ParsedDecimal& d, std::uint8_t precision, Quantity& out) noexcept {
    core::i128 raw9 = 0;
    const core::Status s = detail::to_raw9(d, precision, raw9);
    if (!core::ok(s)) {
      return s;
    }
    if (raw9 < 0 || raw9 > static_cast<core::i128>(kQuantityRawMax)) {
      return core::Status::OutOfRange;
    }
    out = Quantity{static_cast<std::uint64_t>(raw9), precision};
    return core::Status::Ok;
  }

  std::uint64_t raw_ = 0;
  std::uint8_t precision_ = 0;
};

} // namespace jarvis::model
