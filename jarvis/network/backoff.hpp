#pragma once

#include <chrono>
#include <cstdint>

// Reconnect delays (docs/architecture.md section 13.2): exponential from `initial` to `max`,
// with deterministic jitter from a counter so that many connections do not reconnect in step.

namespace jarvis::network {

class Backoff {
public:
  Backoff(std::chrono::milliseconds initial, std::chrono::milliseconds max,
          std::uint64_t seed = 0) noexcept
      : initial_{initial}, max_{max}, state_{seed} {}

  // The delay before the next attempt; grows with each call until reset().
  [[nodiscard]] std::chrono::milliseconds next() noexcept {
    const std::int64_t base = initial_.count() << (attempt_ < 20 ? attempt_ : 20);
    const std::int64_t capped = base < max_.count() ? base : max_.count();
    ++attempt_;
    // +-20% jitter from splitmix64.
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    const std::int64_t span = capped / 5;
    const std::int64_t jitter =
        span > 0 ? static_cast<std::int64_t>(z % static_cast<std::uint64_t>(2 * span + 1)) - span
                 : 0;
    return std::chrono::milliseconds{capped + jitter};
  }
  void reset() noexcept { attempt_ = 0; }
  [[nodiscard]] std::uint32_t attempts() const noexcept { return attempt_; }

private:
  std::chrono::milliseconds initial_;
  std::chrono::milliseconds max_;
  std::uint64_t state_;
  std::uint32_t attempt_ = 0;
};

} // namespace jarvis::network
