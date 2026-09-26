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

// Grammar: [+|-] digits [. digits] [(e|E) [+|-] digits]. Underscores are ignored anywhere, as in
// nautilus. At least one mantissa digit is required.
[[nodiscard]] constexpr core::Status parse_decimal_text(std::string_view text,
                                                        ParsedDecimal& out) noexcept {
  ParsedDecimal result;
  std::size_t pos = 0;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    result.negative = text[pos] == '-';
    ++pos;
  }
  bool any_digit = false;
  bool seen_point = false;
  std::uint32_t fraction_digits = 0;
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
    any_digit = true;
    const auto digit = static_cast<core::u128>(c - '0');
    constexpr core::u128 kLimit = (~static_cast<core::u128>(0) - 9U) / 10U;
    if (result.mantissa > kLimit) {
      return core::Status::Overflow;
    }
    result.mantissa = result.mantissa * 10U + digit;
    if (seen_point) {
      ++fraction_digits;
    }
  }
  if (!any_digit) {
    return core::Status::ParseError;
  }
  std::int64_t exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    bool exponent_negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      exponent_negative = text[pos] == '-';
      ++pos;
    }
    bool exponent_digit = false;
    for (; pos < text.size(); ++pos) {
      const char c = text[pos];
      if (c == '_') {
        continue;
      }
      if (c < '0' || c > '9') {
        return core::Status::ParseError;
      }
      exponent_digit = true;
      exponent = exponent * 10 + (c - '0');
      if (exponent > 64) {
        return core::Status::OutOfRange;
      }
    }
    if (!exponent_digit) {
      return core::Status::ParseError;
    }
    if (exponent_negative) {
      exponent = -exponent;
    }
  }
  if (pos != text.size()) {
    return core::Status::ParseError;
  }
  std::int64_t scale = static_cast<std::int64_t>(fraction_digits) - exponent;
  if (scale < 0) {
    core::u128 factor = 0;
    if (!pow10_u128(static_cast<std::uint32_t>(-scale), factor) ||
        (factor != 0 && result.mantissa > ~static_cast<core::u128>(0) / factor)) {
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

// Converts to a raw value at 10^9 scale for the given precision. Extra fraction digits are
// accepted only when they are zeros; otherwise the conversion would lose information.
[[nodiscard]] constexpr core::Status to_raw9(const ParsedDecimal& d, std::uint8_t precision,
                                             core::i128& raw9) noexcept {
  if (precision > kFixedPrecision) {
    return core::Status::InvalidArgument;
  }
  core::u128 mantissa = d.mantissa;
  if (d.scale > precision) {
    core::u128 divisor = 0;
    if (!pow10_u128(d.scale - precision, divisor)) {
      if (mantissa != 0) {
        return core::Status::PrecisionLoss;
      }
      raw9 = 0;
      return core::Status::Ok;
    }
    if (mantissa % divisor != 0) {
      return core::Status::PrecisionLoss;
    }
    mantissa /= divisor;
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
