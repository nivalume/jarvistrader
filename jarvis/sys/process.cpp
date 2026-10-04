#include "jarvis/sys/process.hpp"

#include <cstdlib>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace jarvis::sys {

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

bool set_env(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
  return ::setenv(name.c_str(), value.c_str(), 1) == 0; // NOLINT(concurrency-mt-unsafe)
#endif
}

bool unset_env(const std::string& name) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), "") == 0; // an empty value removes it
#else
  return ::unsetenv(name.c_str()) == 0; // NOLINT(concurrency-mt-unsafe)
#endif
}

} // namespace jarvis::sys
