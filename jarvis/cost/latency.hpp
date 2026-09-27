#pragma once

#include <array>
#include <concepts>
#include <cstdint>

#include "jarvis/core/rng.hpp"
#include "jarvis/core/time.hpp"

// Delays of the simulated venue (docs/architecture.md sections 11.1 and 12.2). A delay is a pure
// function of (seed, identity, hop): the identity is the input's seq for market data and a hash
// of the ClientOrderId for commands, so a replay draws the same delays.

namespace jarvis::cost {

enum class LatencyHop : std::uint8_t {
  Feed = 0,     // venue event -> strategy (market data)
  Outbound = 1, // command -> venue
  Inbound = 2,  // venue answer -> strategy
};

template <typename L>
concept LatencyModel =
    requires(const L& l, std::uint64_t identity, LatencyHop hop, std::uint32_t attempt) {
      { l.delay(identity, hop, attempt) } -> std::same_as<core::DurationNanos>;
    };

// A fixed base per hop plus a uniform jitter in [0, jitter].
class JitteredLatency {
public:
  JitteredLatency(std::uint64_t seed, std::uint64_t feed_ns, std::uint64_t out_ns,
                  std::uint64_t in_ns, std::uint64_t jitter_ns) noexcept
      : rng_{seed ^ kSalt}, base_{feed_ns, out_ns, in_ns}, jitter_{jitter_ns} {}

  // `attempt` separates several delays of one identity on one hop (the n-th answer to one order).
  [[nodiscard]] core::DurationNanos delay(std::uint64_t identity, LatencyHop hop,
                                          std::uint32_t attempt = 0) const noexcept {
    const auto h = static_cast<std::uint32_t>(hop);
    std::uint64_t d = base_[h];
    if (jitter_ > 0) {
      const std::uint64_t draw = rng_.draw(identity, (h << 24U) | (attempt & 0xFFFFFFU));
      d += jitter_ == UINT64_MAX ? draw : draw % (jitter_ + 1);
    }
    return core::DurationNanos{d};
  }

private:
  // Latency draws use their own Philox key, apart from ctx.rng and event ids.
  static constexpr std::uint64_t kSalt = 0xBB67AE8584CAA73BULL;

  core::CounterRng rng_;
  std::array<std::uint64_t, 3> base_;
  std::uint64_t jitter_;
};

static_assert(LatencyModel<JitteredLatency>);

} // namespace jarvis::cost
