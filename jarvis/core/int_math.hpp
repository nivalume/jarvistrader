#pragma once

#include <array>
#include <cstdint>

#include "jarvis/core/status.hpp"

namespace jarvis::core {

using i128 = __int128;
using u128 = unsigned __int128;

inline constexpr std::array<std::uint64_t, 20> kPow10 = {
    1ULL,
    10ULL,
    100ULL,
    1'000ULL,
    10'000ULL,
    100'000ULL,
    1'000'000ULL,
    10'000'000ULL,
    100'000'000ULL,
    1'000'000'000ULL,
    10'000'000'000ULL,
    100'000'000'000ULL,
    1'000'000'000'000ULL,
    10'000'000'000'000ULL,
    100'000'000'000'000ULL,
    1'000'000'000'000'000ULL,
    10'000'000'000'000'000ULL,
    100'000'000'000'000'000ULL,
    1'000'000'000'000'000'000ULL,
    10'000'000'000'000'000'000ULL,
};

[[nodiscard]] constexpr bool checked_add(std::int64_t a, std::int64_t b,
                                         std::int64_t& out) noexcept {
  return !__builtin_add_overflow(a, b, &out);
}
[[nodiscard]] constexpr bool checked_sub(std::int64_t a, std::int64_t b,
                                         std::int64_t& out) noexcept {
  return !__builtin_sub_overflow(a, b, &out);
}
[[nodiscard]] constexpr bool checked_add(std::uint64_t a, std::uint64_t b,
                                         std::uint64_t& out) noexcept {
  return !__builtin_add_overflow(a, b, &out);
}
[[nodiscard]] constexpr bool checked_sub(std::uint64_t a, std::uint64_t b,
                                         std::uint64_t& out) noexcept {
  return !__builtin_sub_overflow(a, b, &out);
}

// 192-bit unsigned integer, used only as the exact intermediate of mul_div.
struct U192 {
  std::uint64_t lo = 0;
  std::uint64_t mid = 0;
  std::uint64_t hi = 0;
};

[[nodiscard]] constexpr U192 mul_u128_u64(u128 x, std::uint64_t y) noexcept {
  const auto x_lo = static_cast<std::uint64_t>(x);
  const auto x_hi = static_cast<std::uint64_t>(x >> 64U);
  const u128 p0 = static_cast<u128>(x_lo) * y;
  const u128 p1 = static_cast<u128>(x_hi) * y;
  const u128 mid = (p0 >> 64U) + static_cast<u128>(static_cast<std::uint64_t>(p1));
  U192 r;
  r.lo = static_cast<std::uint64_t>(p0);
  r.mid = static_cast<std::uint64_t>(mid);
  r.hi = static_cast<std::uint64_t>(p1 >> 64U) + static_cast<std::uint64_t>(mid >> 64U);
  return r;
}

// Long division of a 192-bit value by a non-zero 64-bit divisor. Quotient truncates.
[[nodiscard]] constexpr U192 div_u192_u64(U192 n, std::uint64_t d) noexcept {
  U192 q;
  u128 cur = n.hi;
  q.hi = static_cast<std::uint64_t>(cur / d);
  u128 rem = cur % d;
  cur = (rem << 64U) | n.mid;
  q.mid = static_cast<std::uint64_t>(cur / d);
  rem = cur % d;
  cur = (rem << 64U) | n.lo;
  q.lo = static_cast<std::uint64_t>(cur / d);
  return q;
}

// out = floor(a * b * c / d), computed exactly through a 192-bit intermediate.
// Returns Status::Overflow when the quotient does not fit in 64 bits and InvalidArgument when
// d is zero.
[[nodiscard]] constexpr Status mul_div_u64(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                           std::uint64_t d, std::uint64_t& out) noexcept {
  if (d == 0) {
    return Status::InvalidArgument;
  }
  const U192 product = mul_u128_u64(static_cast<u128>(a) * b, c);
  const U192 q = div_u192_u64(product, d);
  if (q.hi != 0 || q.mid != 0) {
    return Status::Overflow;
  }
  out = q.lo;
  return Status::Ok;
}

// out = ceil(a * b * c / d), exactly. ceil(ceil(x / p) / q) == ceil(x / (p * q)), so a caller
// can round up in stages without losing the remainder of the first division.
[[nodiscard]] constexpr Status mul_div_u64_up(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                              std::uint64_t d, std::uint64_t& out) noexcept {
  std::uint64_t q = 0;
  const Status s = mul_div_u64(a, b, c, d, q);
  if (!ok(s)) {
    return s;
  }
  const U192 product = mul_u128_u64(static_cast<u128>(a) * b, c);
  const u128 back = static_cast<u128>(q) * d; // q * d <= product < 2^192 and q, d < 2^64
  const u128 low = (static_cast<u128>(product.mid) << 64U) | product.lo;
  if (product.hi == 0 && low == back) {
    out = q;
    return Status::Ok;
  }
  if (q == UINT64_MAX) {
    return Status::Overflow;
  }
  out = q + 1;
  return Status::Ok;
}

[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t v) noexcept {
  return v < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
}

// Signed variant: the magnitude is computed with mul_div_u64 and the sign applied afterwards, so
// the result truncates toward zero. Fails with Overflow if it does not fit in int64.
[[nodiscard]] constexpr Status mul_div_i64(std::int64_t a, std::int64_t b, std::int64_t c,
                                           std::int64_t d, std::int64_t& out) noexcept {
  if (d == 0) {
    return Status::InvalidArgument;
  }
  const int negatives = (a < 0 ? 1 : 0) + (b < 0 ? 1 : 0) + (c < 0 ? 1 : 0) + (d < 0 ? 1 : 0);
  const bool negative = negatives % 2 == 1;
  std::uint64_t q = 0;
  const Status s = mul_div_u64(magnitude(a), magnitude(b), magnitude(c), magnitude(d), q);
  if (!ok(s)) {
    return s;
  }
  if (negative) {
    if (q > static_cast<std::uint64_t>(INT64_MAX) + 1U) {
      return Status::Overflow;
    }
    out = static_cast<std::int64_t>(std::uint64_t{0} - q);
  } else {
    if (q > static_cast<std::uint64_t>(INT64_MAX)) {
      return Status::Overflow;
    }
    out = static_cast<std::int64_t>(q);
  }
  return Status::Ok;
}

// floor(sqrt(v)), exact, by bitwise digit-by-digit extraction.
[[nodiscard]] constexpr std::uint64_t isqrt(u128 v) noexcept {
  u128 result = 0;
  u128 bit = static_cast<u128>(1) << 126U;
  while (bit > v) {
    bit >>= 2U;
  }
  while (bit != 0) {
    if (v >= result + bit) {
      v -= result + bit;
      result = (result >> 1U) + bit;
    } else {
      result >>= 1U;
    }
    bit >>= 2U;
  }
  return static_cast<std::uint64_t>(result);
}

} // namespace jarvis::core
