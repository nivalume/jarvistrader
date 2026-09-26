#pragma once

#include <compare>
#include <cstdint>

#include "jarvis/core/time.hpp"

namespace jarvis::core {

// Strict total order of kernel input events (docs/architecture.md section 5.2, amending
// ADR 0001 decision 2). In backtest `seq` is the row within a source; in sandbox and live it is
// the core thread's ingestion counter, so no two events ever compare equal.
struct EventKey {
  UnixNanos ts;
  std::uint16_t source_id = 0;
  std::uint64_t seq = 0;

  friend constexpr bool operator==(const EventKey&, const EventKey&) noexcept = default;
  friend constexpr auto operator<=>(const EventKey&, const EventKey&) noexcept = default;
};

} // namespace jarvis::core
