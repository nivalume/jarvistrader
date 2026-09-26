#pragma once

#include <array>
#include <string_view>

#include "jarvis/execution/order_fsm.hpp"

// OrderLifecycle's actions and the kernel inputs they stand for (docs/architecture.md 18.2).
// The trace driver and trace-export use this table; the static_asserts fail the build when the
// spec's action or event-kind sets change without this file.
//
//   Plain(k)   an order event of kind k without a quantity   -> the matching model::Order* event
//   Updated(q) OrderUpdated with quantity q                  -> model::OrderUpdated
//   Fill(t, q) OrderFilled of trade t for q                   -> model::OrderFilled
//   Void(t, v) OrderFillVoided of trade t for v               -> model::OrderFillVoided

namespace jarvis::specmap::order_lifecycle {

inline constexpr std::array<std::string_view, 4> kActions = {"Plain", "Updated", "Fill", "Void"};

// The spec's Kinds, in OrderEventKind order.
inline constexpr std::array<std::string_view, 16> kKinds = {
    "DENIED",          "EMULATED",       "RELEASED",       "SUBMITTED",
    "ACCEPTED",        "REJECTED",       "CANCELED",       "EXPIRED",
    "TRIGGERED",       "PENDING_UPDATE", "PENDING_CANCEL", "MODIFY_REJECTED",
    "CANCEL_REJECTED", "UPDATED",        "FILLED",         "FILL_VOIDED"};

static_assert(kKinds.size() == execution::kOrderEventKindCount);

[[nodiscard]] constexpr bool kinds_match() {
  for (std::size_t i = 0; i < kKinds.size(); ++i) {
    if (kKinds[i] != execution::to_string(static_cast<execution::OrderEventKind>(i))) {
      return false;
    }
  }
  return true;
}
static_assert(kinds_match(), "specs/map kinds must follow OrderEventKind");

} // namespace jarvis::specmap::order_lifecycle
