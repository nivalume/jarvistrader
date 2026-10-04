#pragma once

#include <string>

// Error text from the operating system (docs/architecture.md section 3: jarvis/sys holds what
// differs between Linux, macOS and Windows, so that the other layers name no platform API).

namespace jarvis::sys {

// The text of an errno value; thread-safe, unlike std::strerror.
[[nodiscard]] std::string error_text(int errnum);
// errno's current value, as text.
[[nodiscard]] std::string last_error_text();

// The text of a native error code: an errno value on Linux and macOS, a Win32 or Winsock code
// (GetLastError, WSAGetLastError) on Windows.
[[nodiscard]] std::string system_error_text(unsigned long code);
// The calling thread's last native error (errno; GetLastError() on Windows), as text.
[[nodiscard]] std::string last_system_error_text();

} // namespace jarvis::sys
