#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "jarvis/risk/trading_state.hpp"

// TradingState's actions and the kernel inputs they stand for (docs/architecture.md 18.2).
//
//   Trigger(t)  a TradingTrigger: node lifecycle (SYNC_STARTED, SYNCED, DEGRADED, RECOVERED),
//               a post-trade monitor (SOFT_LIMIT, HARD_LIMIT), an admin command
//   Admit(c)    a command of kind c that the gates admit (orders spend one unit of the window)
//   Tick        the rate window rolls over (the input time crosses a multiple of the interval)

namespace jarvis::specmap::trading_state {

inline constexpr std::array<std::string_view, 3> kActions = {"Trigger", "Admit", "Tick"};

inline constexpr std::array<std::string_view, 9> kTriggers = {
    "SYNC_STARTED", "SYNCED",     "DEGRADED",     "RECOVERED",   "SOFT_LIMIT",
    "HARD_LIMIT",   "ADMIN_HALT", "ADMIN_REDUCE", "ADMIN_RESUME"};

inline constexpr std::array<std::string_view, 5> kCommands = {"OPEN", "REDUCE", "MODIFY",
                                                              "MODIFY_UP", "CANCEL"};

[[nodiscard]] constexpr bool names_match() {
  for (std::size_t i = 0; i < kTriggers.size(); ++i) {
    if (kTriggers[i] != risk::to_string(static_cast<risk::TradingTrigger>(i))) {
      return false;
    }
  }
  for (std::size_t i = 0; i < kCommands.size(); ++i) {
    if (kCommands[i] != risk::to_string(static_cast<risk::CommandKind>(i))) {
      return false;
    }
  }
  return true;
}
static_assert(names_match(), "specs/map names must follow TradingTrigger and CommandKind");

} // namespace jarvis::specmap::trading_state
