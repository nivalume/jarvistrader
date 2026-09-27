#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jarvis/core/time.hpp"

// Order rate limits (docs/architecture.md section 10.4), in the kernel so a backtest throttles
// exactly like live. Binance counts orders in fixed windows aligned to the clock (10 s and
// 1 min for USD-M); a window here holds at most `limit` orders between two multiples of its
// interval. Time is the input's ts, so the state is a function of the inputs and needs no timer.
// The configured limits sit below the venue's, leaving room for reconciliation and retries.

namespace jarvis::risk {

struct RateWindow {
  std::uint64_t interval_ns = 0;
  std::uint32_t limit = 0;
};

class RateLimiter {
public:
  static constexpr std::size_t kMaxWindows = 4;

  RateLimiter() noexcept = default;
  explicit RateLimiter(std::span<const RateWindow> windows) noexcept {
    for (const RateWindow& w : windows) {
      if (count_ < kMaxWindows && w.interval_ns > 0) {
        windows_[count_++] = State{w, UINT64_MAX, 0};
      }
    }
  }

  // Orders still allowed now (UINT32_MAX without windows).
  [[nodiscard]] std::uint32_t remaining(core::UnixNanos now) noexcept {
    std::uint32_t left = UINT32_MAX;
    for (std::size_t i = 0; i < count_; ++i) {
      State& s = roll(i, now);
      const std::uint32_t room = s.used >= s.window.limit ? 0 : s.window.limit - s.used;
      left = room < left ? room : left;
    }
    return left;
  }

  // Takes `cost` from every window, or nothing when any window lacks room.
  [[nodiscard]] bool try_acquire(core::UnixNanos now, std::uint32_t cost = 1) noexcept {
    if (remaining(now) < cost) {
      return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      windows_[i].used += cost;
    }
    return true;
  }

  // The venue's count for the window of length `interval_ns` (RateLimitFeedback): ours rises to
  // at least `used`, never falls, so orders the venue counted that we did not (another
  // session, a retry) are not sent past its limit.
  void feedback(core::UnixNanos now, std::uint64_t interval_ns, std::uint32_t used) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (windows_[i].window.interval_ns == interval_ns) {
        State& s = roll(i, now);
        s.used = used > s.used ? used : s.used;
      }
    }
  }

  [[nodiscard]] std::size_t windows() const noexcept { return count_; }

private:
  struct State {
    RateWindow window;
    std::uint64_t index = UINT64_MAX; // which interval the count belongs to
    std::uint32_t used = 0;
  };

  State& roll(std::size_t i, core::UnixNanos now) noexcept {
    State& s = windows_[i];
    const std::uint64_t index = now.value() / s.window.interval_ns;
    if (index != s.index) {
      s.index = index;
      s.used = 0;
    }
    return s;
  }

  std::array<State, kMaxWindows> windows_{};
  std::size_t count_ = 0;
};

} // namespace jarvis::risk
