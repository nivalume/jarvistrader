#pragma once

#include <cstdint>
#include <span>

namespace jarvis::testkit {

// Deterministic splitmix64 stream for property tests.
//
// Test-only. Kernel code must use the counter-based generators in jarvis/core and never a
// stateful generator (docs/adr/0001-deterministic-kernel.md, decision 4).
class Gen {
public:
  explicit Gen(std::uint64_t seed) noexcept : state_{seed} {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  // Uniform in [0, bound). Requires bound > 0. Lemire's multiply-shift with rejection, so the
  // result is unbiased.
  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    unsigned __int128 product = static_cast<unsigned __int128>(next()) * bound;
    auto low = static_cast<std::uint64_t>(product);
    if (low < bound) {
      const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
      while (low < threshold) {
        product = static_cast<unsigned __int128>(next()) * bound;
        low = static_cast<std::uint64_t>(product);
      }
    }
    return static_cast<std::uint64_t>(product >> 64U);
  }

  // Uniform in the closed interval [lo, hi]. Requires lo <= hi.
  [[nodiscard]] std::int64_t range(std::int64_t lo, std::int64_t hi) noexcept {
    const std::uint64_t width = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    const std::uint64_t offset = width == UINT64_MAX ? next() : below(width + 1U);
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + offset);
  }

  // Uniform in the closed interval [lo, hi] for unsigned values. Requires lo <= hi.
  [[nodiscard]] std::uint64_t range_u(std::uint64_t lo, std::uint64_t hi) noexcept {
    const std::uint64_t width = hi - lo;
    return lo + (width == UINT64_MAX ? next() : below(width + 1U));
  }

  [[nodiscard]] bool coin() noexcept { return (next() >> 63U) != 0U; }

  // True with probability numerator / denominator. Requires denominator > 0.
  [[nodiscard]] bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return below(denominator) < numerator;
  }

  // Requires a non-empty span.
  template <typename T> [[nodiscard]] const T& pick(std::span<const T> items) noexcept {
    return items[static_cast<std::size_t>(below(items.size()))];
  }

  // An independent stream derived from this one.
  [[nodiscard]] Gen split() noexcept { return Gen{next()}; }

private:
  std::uint64_t state_;
};

} // namespace jarvis::testkit
