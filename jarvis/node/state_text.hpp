#pragma once

#include <string>

#include "jarvis/strategy/context.hpp"

namespace jarvis::node {

// A readable summary of the kernel state after the last step, for `replay --dump-state`:
// the current key, instruments and their books, subscriptions, features with their last
// values, bar types, active timers and halted strategies. Stable line format, one fact per line.
[[nodiscard]] std::string kernel_state_text(const strategy::KernelServices& kernel);

} // namespace jarvis::node
