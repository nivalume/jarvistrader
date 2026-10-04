#pragma once

#include <cstdint>
#include <string>

// The process and its environment, the same on every platform.

namespace jarvis::sys {

[[nodiscard]] std::uint64_t process_id() noexcept;

// Sets or removes an environment variable of this process (tests and tools; not thread-safe,
// like the platform calls behind them). False when the platform refuses.
bool set_env(const std::string& name, const std::string& value);
bool unset_env(const std::string& name);

} // namespace jarvis::sys
