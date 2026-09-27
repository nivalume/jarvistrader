#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

#include "jarvis/core/status.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"

// Replays the orders of an event log through a fresh OMS, with the kernel's own code, so that
// tools reading a log see each order as the kernel saw it (jarvis trace-export, jarvis report).
//
// The log holds the venue's order events as inputs and the kernel's commands as outputs; the
// events the kernel applied itself are recovered from the commands, as strategy::Trading does:
//
//   SubmitOrder   the order is created with its terms, then SUBMITTED
//   OrderDenied   the order is created (quantity 1: the denial does not carry it), then DENIED
//   ModifyOrder   PENDING_UPDATE
//   CancelOrder   PENDING_CANCEL
//   order event   execution::apply_order_event, as on_venue_event does
//
// Events of orders the log never created are not applied (known = false).

namespace jarvis::node {

namespace detail {
template <typename T, typename V> struct is_alternative : std::false_type {};
template <typename T, typename... A>
struct is_alternative<T, std::variant<A...>> : std::bool_constant<(std::is_same_v<T, A> || ...)> {};
} // namespace detail

// Whether a model::Event alternative is an order event.
template <typename T>
inline constexpr bool kIsOrderEvent = detail::is_alternative<T, model::OrderEvent>::value;

struct ReplayOutcome {
  bool known = false;   // the log created the order
  bool applied = false; // the OMS applied the event
  std::uint32_t index = execution::kNoIndex;
  execution::OrderEventKind kind = execution::OrderEventKind::Denied;
};

class OrderReplay {
public:
  // Capacities from count(): the OMS never evicts.
  OrderReplay(std::size_t orders, std::size_t fills)
      : oms_{static_cast<std::uint32_t>(orders + 1), static_cast<std::uint32_t>(fills + 1)} {}

  // A kernel command; known = false for outputs that are not order commands.
  [[nodiscard]] ReplayOutcome on_output(const model::Output& output);
  [[nodiscard]] ReplayOutcome on_event(const model::OrderEvent& event);

  [[nodiscard]] const execution::Oms& oms() const noexcept { return oms_; }

  // Upper bounds of the orders and fills in the log in `directory`.
  [[nodiscard]] static core::Status count(const std::string& directory, std::size_t& orders,
                                          std::size_t& fills);

private:
  ReplayOutcome create(const model::SubmitOrder* submit, const model::ClientOrderId& id,
                       execution::OrderEventKind kind);
  ReplayOutcome internal(const model::ClientOrderId& id, execution::OrderEventKind kind);

  execution::Oms oms_;
};

} // namespace jarvis::node
