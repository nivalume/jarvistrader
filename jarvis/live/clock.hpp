#pragma once

#include "jarvis/core/clock.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/live/raw_frames.hpp"

// The live node's clock (docs/architecture.md section 4.1): UTC nanoseconds from the steady
// clock, anchored to the wall clock once. It never goes backwards, so the core's input keys and
// the IO threads' arrival stamps (ArrivalClock::utc_of) share one time line even when NTP adjusts
// the wall clock. Reads are thread-safe.

namespace jarvis::live {

class MonotonicClock {
public:
  explicit MonotonicClock(const ArrivalClock& anchor) noexcept : anchor_{&anchor} {}

  [[nodiscard]] core::UnixNanos now() const noexcept { return core::UnixNanos{anchor_->now()}; }
  [[nodiscard]] const ArrivalClock& anchor() const noexcept { return *anchor_; }

private:
  const ArrivalClock* anchor_;
};

static_assert(core::Clock<MonotonicClock>);

} // namespace jarvis::live
