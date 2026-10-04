#include "jarvis/sys/time.hpp"

namespace jarvis::sys {

std::tm utc_calendar(std::time_t time) noexcept {
  std::tm tm{};
#if defined(_WIN32)
  static_cast<void>(gmtime_s(&tm, &time));
#else
  static_cast<void>(gmtime_r(&time, &tm));
#endif
  return tm;
}

} // namespace jarvis::sys
