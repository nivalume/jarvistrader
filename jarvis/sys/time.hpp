#pragma once

#include <ctime>

// Calendar time, the same on every platform.

namespace jarvis::sys {

// The UTC calendar fields of a time_t (gmtime_r, or gmtime_s on Windows); thread-safe.
[[nodiscard]] std::tm utc_calendar(std::time_t time) noexcept;

} // namespace jarvis::sys
