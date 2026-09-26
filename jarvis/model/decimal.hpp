#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"

namespace jarvis::model {

// nautilus standard-precision fixed point: raw values are integers scaled by 10^9
// (docs/architecture.md section 6.1). Precision only affects formatting.
inline constexpr std::uint8_t kFixedPrecision = 9;
inline constexpr std::int64_t kFixedScalar = 1'000'000'000;
// Longest text of a formatted value: sign, 11 integer digits, point, 9 fraction digits.
inline constexpr std::size_t kMaxDecimalText = 24;

namespace detail {

// A decimal read from text: value = (negative ? -1 : 1) * mantissa * 10^-scale.
struct ParsedDecimal {
  core::u128 mantissa = 0;
  bool negative = false;
  std::uint32_t scale = 0;
};

[[nodiscard]] constexpr bool pow10_u128(std::uint32_t exponent, core::u128& out) noexcept {
  if (exponent > 38) {
    return false;
  }
  core::u128 value = 1;
  for (std::uint32_t i = 0; i < exponent; ++i) {
    value *= 10U;
  }
  out = value;
  return true;
}

// "[+|-] digits [. digits]" at `pos`, underscores ignored: sign, mantissa and fraction digits.
[[nodiscard]] constexpr core::Status parse_mantissa(std::string_view text, std::size_t& pos,
                                                    ParsedDecimal& result,
                                                    std::uint32_t& fraction_digits) noexcept {
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    result.negative = text[pos] == '-';
    ++pos;
  }
  bool any_digit = false;
  bool seen_point = false;
  constexpr core::u128 kLimit = (~static_cast<core::u128>(0) - 9U) / 10U;
  for (; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '_') {
      continue;
    }
    if (c == '.') {
      if (seen_point) {
        return core::Status::ParseError;
      }
      seen_point = true;
      continue;
    }
    if (c < '0' || c > '9') {
      break;
    }
    if (result.mantissa > kLimit) {
      return core::Status::Overflow;
    }
    any_digit = true;
    result.mantissa = result.mantissa * 10U + static_cast<core::u128>(c - '0');
    fraction_digits += seen_point ? 1U : 0U;
  }
  return any_digit ? core::Status::Ok : core::Status::ParseError;
}

// Optional "(e|E) [+|-] digits" at `pos`, underscores ignored. |exponent| is capped at 64.
[[nodiscard]] constexpr core::Status parse_exponent(std::string_view text, std::size_t& pos,
                                                    std::int64_t& exponent) noexcept {
  exponent = 0;
  if (pos >= text.size() || (text[pos] != 'e' && text[pos] != 'E')) {
    return core::Status::Ok;
  }
  ++pos;
  bool negative = false;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    negative = text[pos] == '-';
    ++pos;
  }
  bool any_digit = false;
  for (; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '_') {
      continue;
    }
    if (c < '0' || c > '9') {
      return core::Status::ParseError;
    }
    any_digit = true;
    exponent = exponent * 10 + (c - '0');
    if (exponent > 64) {
      return core::Status::OutOfRange;
    }
  }
  if (!any_digit) {
    return core::Status::ParseError;
  }
  exponent = negative ? -exponent : exponent;
  return core::Status::Ok;
}

// Grammar: [+|-] digits [. digits] [(e|E) [+|-] digits]. Underscores are ignored anywhere, as in
// nautilus. At least one mantissa digit is required.
[[nodiscard]] constexpr core::Status parse_decimal_text(std::string_view text,
                                                        ParsedDecimal& out) noexcept {
  ParsedDecimal result;
  std::size_t pos = 0;
  std::uint32_t fraction_digits = 0;
  std::int64_t exponent = 0;
  core::Status s = parse_mantissa(text, pos, result, fraction_digits);
  if (core::ok(s)) {
    s = parse_exponent(text, pos, exponent);
  }
  if (!core::ok(s)) {
    return s;
  }
  if (pos != text.size()) {
    return core::Status::ParseError;
  }
  std::int64_t scale = static_cast<std::int64_t>(fraction_digits) - exponent;
  if (scale < 0) {
    core::u128 factor = 0;
    if (!pow10_u128(static_cast<std::uint32_t>(-scale), factor) ||
        result.mantissa > ~static_cast<core::u128>(0) / factor) {
      return core::Status::Overflow;
    }
    result.mantissa *= factor;
    scale = 0;
  }
  result.scale = static_cast<std::uint32_t>(scale);
  out = result;
  return core::Status::Ok;
}

// Number of fraction digits the text implies, as nautilus infers precision.
[[nodiscard]] constexpr std::uint32_t implied_precision(const ParsedDecimal& d) noexcept {
  return d.scale;
}

// Converts to a raw value at 10^9 scale for the given precision. Fraction digits beyond the
// precision are rounded half to even, as nautilus does in `mantissa_exponent_to_fixed_i128`
// (Money::from_str, Price::from_decimal_dp, from_mantissa_exponent).
[[nodiscard]] constexpr core::Status to_raw9(const ParsedDecimal& d, std::uint8_t precision,
                                             core::i128& raw9) noexcept {
  if (precision > kFixedPrecision) {
    return core::Status::InvalidArgument;
  }
  core::u128 mantissa = d.mantissa;
  if (d.scale > precision) {
    core::u128 divisor = 0;
    if (!pow10_u128(d.scale - precision, divisor)) {
      raw9 = 0; // more than 38 excess digits: every representable mantissa rounds to zero
      return core::Status::Ok;
    }
    const core::u128 quotient = mantissa / divisor;
    const core::u128 remainder = mantissa % divisor;
    const core::u128 half = divisor / 2U;
    const bool round_up = remainder > half || (remainder == half && quotient % 2U != 0U);
    mantissa = quotient + (round_up ? 1U : 0U);
  } else {
    core::u128 factor = 0;
    static_cast<void>(pow10_u128(precision - d.scale, factor));
    if (mantissa > ~static_cast<core::u128>(0) / factor) {
      return core::Status::Overflow;
    }
    mantissa *= factor;
  }
  const core::u128 scale9 = core::kPow10[kFixedPrecision - precision];
  constexpr core::u128 kMaxMagnitude = static_cast<core::u128>(1) << 126U;
  if (mantissa > kMaxMagnitude / scale9) {
    return core::Status::Overflow;
  }
  const auto magnitude = static_cast<core::i128>(mantissa * scale9);
  raw9 = d.negative ? -magnitude : magnitude;
  return core::Status::Ok;
}

// Writes raw9 (10^9 scale) with exactly `precision` fraction digits. Digits below the precision
// are truncated toward zero; callers keep raw values multiples of 10^(9 - precision).
[[nodiscard]] constexpr core::Status format_raw9(core::i128 raw9, std::uint8_t precision,
                                                 std::span<char> out,
                                                 std::size_t& written) noexcept {
  if (precision > kFixedPrecision) {
    return core::Status::InvalidArgument;
  }
  const bool negative = raw9 < 0;
  core::u128 magnitude = negative ? static_cast<core::u128>(-raw9) : static_cast<core::u128>(raw9);
  magnitude /= core::kPow10[kFixedPrecision - precision];
  const core::u128 unit = core::kPow10[precision];
  core::u128 integer = magnitude / unit;
  core::u128 fraction = magnitude % unit;

  char digits[48]{};
  std::size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + static_cast<int>(integer % 10U));
    integer /= 10U;
  } while (integer != 0 && count < sizeof(digits));

  const bool show_sign = negative && (magnitude != 0);
  const std::size_t needed = (show_sign ? 1U : 0U) + count + (precision > 0 ? 1U + precision : 0U);
  if (needed > out.size()) {
    return core::Status::OutOfRange;
  }
  std::size_t pos = 0;
  if (show_sign) {
    out[pos++] = '-';
  }
  while (count > 0) {
    out[pos++] = digits[--count];
  }
  if (precision > 0) {
    out[pos++] = '.';
    for (std::size_t i = precision; i > 0; --i) {
      out[pos + i - 1] = static_cast<char>('0' + static_cast<int>(fraction % 10U));
      fraction /= 10U;
    }
    pos += precision;
  }
  written = pos;
  return core::Status::Ok;
}

} // namespace detail

} // namespace jarvis::model
