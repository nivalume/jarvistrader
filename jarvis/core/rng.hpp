#pragma once

#include <array>
#include <cstdint>

namespace jarvis::core {

// Counter-based random numbers (docs/adr/0001-deterministic-kernel.md, decision 4). A draw is a
// pure function of (seed, identity, hop, index), so results never depend on call order and any
// draw can be recomputed in isolation.

// splitmix64 finalizer: a bijective 64-bit mix.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

using PhiloxCounter = std::array<std::uint32_t, 4>;
using PhiloxKey = std::array<std::uint32_t, 2>;

// Philox4x32-10 (Salmon et al., "Parallel random numbers: as easy as 1, 2, 3", SC11).
[[nodiscard]] constexpr PhiloxCounter philox4x32_10(PhiloxCounter ctr, PhiloxKey key) noexcept {
  constexpr std::uint64_t kM0 = 0xD2511F53U;
  constexpr std::uint64_t kM1 = 0xCD9E8D57U;
  constexpr std::uint32_t kW0 = 0x9E3779B9U;
  constexpr std::uint32_t kW1 = 0xBB67AE85U;
  for (int round = 0; round < 10; ++round) {
    if (round > 0) {
      key[0] += kW0;
      key[1] += kW1;
    }
    const std::uint64_t p0 = kM0 * ctr[0];
    const std::uint64_t p1 = kM1 * ctr[2];
    const auto hi0 = static_cast<std::uint32_t>(p0 >> 32U);
    const auto lo0 = static_cast<std::uint32_t>(p0);
    const auto hi1 = static_cast<std::uint32_t>(p1 >> 32U);
    const auto lo1 = static_cast<std::uint32_t>(p1);
    ctr = PhiloxCounter{hi1 ^ ctr[1] ^ key[0], lo1, hi0 ^ ctr[3] ^ key[1], lo0};
  }
  return ctr;
}

// Draws keyed by (seed, identity, hop, index). `identity` names the thing being randomized (an
// order, an event sequence number), `hop` the purpose (outbound latency, inbound latency, ...),
// and `index` distinguishes several draws for the same purpose.
class CounterRng {
public:
  explicit constexpr CounterRng(std::uint64_t seed) noexcept
      : key_{static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32U)} {}

  [[nodiscard]] constexpr std::uint64_t draw(std::uint64_t identity, std::uint32_t hop,
                                             std::uint32_t index = 0) const noexcept {
    const PhiloxCounter out = philox4x32_10(
        PhiloxCounter{static_cast<std::uint32_t>(identity),
                      static_cast<std::uint32_t>(identity >> 32U), hop, index},
        key_);
    return (static_cast<std::uint64_t>(out[1]) << 32U) | out[0];
  }

  // Uniform in [0, bound) by multiply-high. Bias is at most bound / 2^64, far below anything a
  // latency model can observe; requires bound > 0.
  [[nodiscard]] constexpr std::uint64_t below(std::uint64_t bound, std::uint64_t identity,
                                              std::uint32_t hop,
                                              std::uint32_t index = 0) const noexcept {
    const unsigned __int128 product =
        static_cast<unsigned __int128>(draw(identity, hop, index)) * bound;
    return static_cast<std::uint64_t>(product >> 64U);
  }

private:
  PhiloxKey key_;
};

} // namespace jarvis::core
