#include "jarvis/sys/error.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace jarvis::sys {

std::string error_text(int errnum) {
  std::array<char, 256> buffer{};
#if defined(_WIN32)
  if (strerror_s(buffer.data(), buffer.size(), errnum) != 0) {
    return "error " + std::to_string(errnum);
  }
  return std::string{buffer.data()};
#elif defined(__GLIBC__) && defined(_GNU_SOURCE)
  // The GNU variant returns the text, which may or may not be in `buffer`.
  return std::string{strerror_r(errnum, buffer.data(), buffer.size())};
#else
  if (strerror_r(errnum, buffer.data(), buffer.size()) != 0) {
    return "error " + std::to_string(errnum);
  }
  return std::string{buffer.data()};
#endif
}

std::string last_error_text() { return error_text(errno); }

std::string system_error_text(unsigned long code) {
#if defined(_WIN32)
  char* text = nullptr;
  const DWORD n = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(code), 0,
      reinterpret_cast<char*>(&text), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      0, nullptr);
  std::string out = n > 0 && text != nullptr ? std::string{text, n} : std::string{};
  ::LocalFree(text);
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  return out.empty() ? "error " + std::to_string(code) : out;
#else
  return error_text(static_cast<int>(code));
#endif
}

std::string last_system_error_text() {
#if defined(_WIN32)
  return system_error_text(::GetLastError());
#else
  return error_text(errno);
#endif
}

} // namespace jarvis::sys
