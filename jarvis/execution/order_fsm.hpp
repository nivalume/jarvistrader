#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/order_events.hpp"

// The order state machine's transition relation (docs/architecture.md section 8.1): nautilus
// OrderStatus::transition at cd417b80 plus SUBMITTED --EXPIRED--> EXPIRED. The TLA+ spec
// specs/tla/OrderLifecycle.tla is the source of truth; tests/cpp/test_execution.cpp fails when
// this table and the spec's Transitions set differ. The apply-phase rules (fills, previous
// status) live in jarvis/execution/order.hpp.

namespace jarvis::execution {

using model::OrderStatus;

// Every order event except OrderInitialized, which creates the order instead of moving it.
enum class OrderEventKind : std::uint8_t {
  Denied,
  Emulated,
  Released,
  Submitted,
  Accepted,
  Rejected,
  Canceled,
  Expired,
  Triggered,
  PendingUpdate,
  PendingCancel,
  ModifyRejected,
  CancelRejected,
  Updated,
  Filled,
  FillVoided,
};
inline constexpr std::size_t kOrderEventKindCount = 16;

[[nodiscard]] constexpr std::string_view to_string(OrderEventKind k) noexcept {
  constexpr std::array<std::string_view, kOrderEventKindCount> kNames = {
      "DENIED",          "EMULATED",       "RELEASED",       "SUBMITTED",
      "ACCEPTED",        "REJECTED",       "CANCELED",       "EXPIRED",
      "TRIGGERED",       "PENDING_UPDATE", "PENDING_CANCEL", "MODIFY_REJECTED",
      "CANCEL_REJECTED", "UPDATED",        "FILLED",         "FILL_VOIDED"};
  return kNames[static_cast<std::size_t>(k)];
}

struct OrderTransition {
  OrderStatus from;
  OrderEventKind kind;
  OrderStatus to;
};

namespace detail {
using S = OrderStatus;
using K = OrderEventKind;
} // namespace detail

// clang-format off
inline constexpr auto kOrderTransitions = std::to_array<OrderTransition>({
    {detail::S::Initialized, detail::K::Denied, detail::S::Denied},
    {detail::S::Initialized, detail::K::Emulated, detail::S::Emulated},
    {detail::S::Initialized, detail::K::Released, detail::S::Released},
    {detail::S::Initialized, detail::K::Submitted, detail::S::Submitted},
    {detail::S::Initialized, detail::K::Rejected, detail::S::Rejected},
    {detail::S::Initialized, detail::K::Accepted, detail::S::Accepted},
    {detail::S::Initialized, detail::K::Canceled, detail::S::Canceled},
    {detail::S::Initialized, detail::K::Expired, detail::S::Expired},
    {detail::S::Initialized, detail::K::Triggered, detail::S::Triggered},
    {detail::S::Initialized, detail::K::Updated, detail::S::Initialized},
    {detail::S::Emulated, detail::K::Canceled, detail::S::Canceled},
    {detail::S::Emulated, detail::K::Expired, detail::S::Expired},
    {detail::S::Emulated, detail::K::Updated, detail::S::Emulated},
    {detail::S::Emulated, detail::K::Released, detail::S::Released},
    {detail::S::Released, detail::K::Submitted, detail::S::Submitted},
    {detail::S::Released, detail::K::Denied, detail::S::Denied},
    {detail::S::Released, detail::K::Canceled, detail::S::Canceled},
    {detail::S::Released, detail::K::Updated, detail::S::Released},
    {detail::S::Submitted, detail::K::PendingUpdate, detail::S::PendingUpdate},
    {detail::S::Submitted, detail::K::PendingCancel, detail::S::PendingCancel},
    {detail::S::Submitted, detail::K::Rejected, detail::S::Rejected},
    {detail::S::Submitted, detail::K::Canceled, detail::S::Canceled},
    {detail::S::Submitted, detail::K::Expired, detail::S::Expired},
    {detail::S::Submitted, detail::K::Accepted, detail::S::Accepted},
    {detail::S::Submitted, detail::K::Updated, detail::S::Submitted},
    {detail::S::Submitted, detail::K::Filled, detail::S::Filled},
    {detail::S::Accepted, detail::K::Rejected, detail::S::Rejected},
    {detail::S::Accepted, detail::K::PendingUpdate, detail::S::PendingUpdate},
    {detail::S::Accepted, detail::K::PendingCancel, detail::S::PendingCancel},
    {detail::S::Accepted, detail::K::CancelRejected, detail::S::Accepted},
    {detail::S::Accepted, detail::K::Canceled, detail::S::Canceled},
    {detail::S::Accepted, detail::K::Triggered, detail::S::Triggered},
    {detail::S::Accepted, detail::K::Updated, detail::S::Accepted},
    {detail::S::Accepted, detail::K::Expired, detail::S::Expired},
    {detail::S::Accepted, detail::K::Filled, detail::S::Filled},
    {detail::S::Accepted, detail::K::FillVoided, detail::S::Accepted},
    {detail::S::Canceled, detail::K::Filled, detail::S::Filled},
    {detail::S::Canceled, detail::K::FillVoided, detail::S::Canceled},
    {detail::S::Canceled, detail::K::Updated, detail::S::Canceled},
    {detail::S::PendingUpdate, detail::K::Rejected, detail::S::Rejected},
    {detail::S::PendingUpdate, detail::K::Accepted, detail::S::Accepted},
    {detail::S::PendingUpdate, detail::K::Canceled, detail::S::Canceled},
    {detail::S::PendingUpdate, detail::K::Expired, detail::S::Expired},
    {detail::S::PendingUpdate, detail::K::Triggered, detail::S::Triggered},
    {detail::S::PendingUpdate, detail::K::Submitted, detail::S::PendingUpdate},
    {detail::S::PendingUpdate, detail::K::PendingUpdate, detail::S::PendingUpdate},
    {detail::S::PendingUpdate, detail::K::PendingCancel, detail::S::PendingCancel},
    {detail::S::PendingUpdate, detail::K::ModifyRejected, detail::S::PendingUpdate},
    {detail::S::PendingUpdate, detail::K::Updated, detail::S::PendingUpdate},
    {detail::S::PendingUpdate, detail::K::Filled, detail::S::Filled},
    {detail::S::PendingUpdate, detail::K::FillVoided, detail::S::PendingUpdate},
    {detail::S::PendingCancel, detail::K::Rejected, detail::S::Rejected},
    {detail::S::PendingCancel, detail::K::PendingCancel, detail::S::PendingCancel},
    {detail::S::PendingCancel, detail::K::ModifyRejected, detail::S::PendingCancel},
    {detail::S::PendingCancel, detail::K::CancelRejected, detail::S::PendingCancel},
    {detail::S::PendingCancel, detail::K::Canceled, detail::S::Canceled},
    {detail::S::PendingCancel, detail::K::Expired, detail::S::Expired},
    {detail::S::PendingCancel, detail::K::Accepted, detail::S::Accepted},
    {detail::S::PendingCancel, detail::K::Updated, detail::S::PendingCancel},
    {detail::S::PendingCancel, detail::K::Filled, detail::S::Filled},
    {detail::S::PendingCancel, detail::K::FillVoided, detail::S::PendingCancel},
    {detail::S::Triggered, detail::K::Rejected, detail::S::Rejected},
    {detail::S::Triggered, detail::K::PendingUpdate, detail::S::PendingUpdate},
    {detail::S::Triggered, detail::K::PendingCancel, detail::S::PendingCancel},
    {detail::S::Triggered, detail::K::Canceled, detail::S::Canceled},
    {detail::S::Triggered, detail::K::Expired, detail::S::Expired},
    {detail::S::Triggered, detail::K::Filled, detail::S::Filled},
    {detail::S::Triggered, detail::K::Updated, detail::S::Triggered},
    {detail::S::Triggered, detail::K::FillVoided, detail::S::Triggered},
    {detail::S::PartiallyFilled, detail::K::PendingUpdate, detail::S::PendingUpdate},
    {detail::S::PartiallyFilled, detail::K::PendingCancel, detail::S::PendingCancel},
    {detail::S::PartiallyFilled, detail::K::Canceled, detail::S::Canceled},
    {detail::S::PartiallyFilled, detail::K::Expired, detail::S::Expired},
    {detail::S::PartiallyFilled, detail::K::Filled, detail::S::Filled},
    {detail::S::PartiallyFilled, detail::K::Accepted, detail::S::Accepted},
    {detail::S::PartiallyFilled, detail::K::Updated, detail::S::PartiallyFilled},
    {detail::S::PartiallyFilled, detail::K::FillVoided, detail::S::PartiallyFilled},
    {detail::S::Filled, detail::K::FillVoided, detail::S::Voided},
    {detail::S::Filled, detail::K::Updated, detail::S::Filled},
    {detail::S::Expired, detail::K::FillVoided, detail::S::Expired},
    {detail::S::Expired, detail::K::Updated, detail::S::Expired},
    {detail::S::Voided, detail::K::FillVoided, detail::S::Voided},
});
// clang-format on

// The status `from` moves to on `kind`, or false when the table has no such edge.
[[nodiscard]] constexpr bool next_status(OrderStatus from, OrderEventKind kind,
                                         OrderStatus& to) noexcept {
  for (const OrderTransition& t : kOrderTransitions) {
    if (t.from == from && t.kind == kind) {
      to = t.to;
      return true;
    }
  }
  return false;
}

// An order the venue may still fill (nautilus is_open).
[[nodiscard]] constexpr bool is_open(OrderStatus s) noexcept {
  return s == OrderStatus::Accepted || s == OrderStatus::Triggered ||
         s == OrderStatus::PendingUpdate || s == OrderStatus::PendingCancel ||
         s == OrderStatus::PartiallyFilled || s == OrderStatus::Emulated ||
         s == OrderStatus::Released || s == OrderStatus::Submitted;
}

// A request is out and its answer has not arrived.
[[nodiscard]] constexpr bool is_in_flight(OrderStatus s) noexcept {
  return s == OrderStatus::Submitted || s == OrderStatus::PendingUpdate ||
         s == OrderStatus::PendingCancel;
}

[[nodiscard]] constexpr bool is_closed(OrderStatus s) noexcept {
  return s == OrderStatus::Denied || s == OrderStatus::Rejected || s == OrderStatus::Canceled ||
         s == OrderStatus::Expired || s == OrderStatus::Filled || s == OrderStatus::Voided;
}

[[nodiscard]] constexpr bool is_pending(OrderStatus s) noexcept {
  return s == OrderStatus::PendingUpdate || s == OrderStatus::PendingCancel;
}

// The kind of an order event; false for OrderInitialized. The OrderEvent alternatives follow
// OrderInitialized in the order of OrderEventKind (checked below).
[[nodiscard]] constexpr bool kind_of(const model::OrderEvent& event, OrderEventKind& out) noexcept {
  const std::size_t i = event.index();
  if (i == 0 || i > kOrderEventKindCount) {
    return false;
  }
  out = static_cast<OrderEventKind>(i - 1);
  return true;
}

namespace detail {
template <typename E> constexpr std::size_t event_index() { return model::OrderEvent{E{}}.index(); }
static_assert(std::variant_size_v<model::OrderEvent> == kOrderEventKindCount + 1);
static_assert(event_index<model::OrderDenied>() == 1 + static_cast<std::size_t>(K::Denied));
static_assert(event_index<model::OrderSubmitted>() == 1 + static_cast<std::size_t>(K::Submitted));
static_assert(event_index<model::OrderPendingCancel>() ==
              1 + static_cast<std::size_t>(K::PendingCancel));
static_assert(event_index<model::OrderUpdated>() == 1 + static_cast<std::size_t>(K::Updated));
static_assert(event_index<model::OrderFillVoided>() == 1 + static_cast<std::size_t>(K::FillVoided));
} // namespace detail

} // namespace jarvis::execution
