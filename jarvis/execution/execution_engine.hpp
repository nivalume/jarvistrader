#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "jarvis/core/status.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/order_events.hpp"

// The return path of docs/architecture.md section 9.1: an order event from the venue (the
// adapter in live, the simulated exchange in backtest) advances the order in the OMS. Only events
// the OMS accepts reach the strategy; the others are counted and dropped:
//
//   UnknownOrder   no order with this ClientOrderId (never ours, or evicted long ago);
//   Refused        the state machine has no such transition, or the quantities do not fit
//                  (an overfill, a void of more than was filled);
//   DuplicateFill  a second report of a trade already applied (TRADE_LITE and
//                  ORDER_TRADE_UPDATE, section 8.3); the caller books its commission when
//                  the first report was Lite (Oms::take_pending_commission);
//   Stale          a status event older (by venue time) than the latest venue event applied to
//                  the order, delivered out of order: applying it would undo a newer change.
//                  Fills are never stale: a trade counts whatever its time, once.
//
// OrderInitialized is not accepted from outside: orders enter the OMS through submit, and
// adopting a venue order is reconciliation's (section 15).

namespace jarvis::execution {

enum class EventOutcome : std::uint8_t { Applied, UnknownOrder, Refused, DuplicateFill, Stale };

namespace detail {

// The first venue order id an event carries is kept; events without one leave it alone.
template <typename E> void note_venue_order_id(OrderRecord& r, const E& e) noexcept {
  if constexpr (requires { e.venue_order_id; }) {
    using V = std::remove_cvref_t<decltype(e.venue_order_id)>;
    if (r.venue_order_id) {
      return;
    }
    if constexpr (std::is_same_v<V, model::VenueOrderId>) {
      if (!e.venue_order_id.empty()) {
        r.venue_order_id = e.venue_order_id;
      }
    } else if (e.venue_order_id && !e.venue_order_id->empty()) {
      r.venue_order_id = e.venue_order_id;
    }
  }
}

template <typename E>
core::Status apply_to(Oms& oms, std::uint32_t index, OrderEventKind kind, const E& e) noexcept {
  if constexpr (std::is_same_v<E, model::OrderFilled>) {
    const bool lite = (e.info_flags & static_cast<std::uint8_t>(model::FillInfo::Lite)) != 0;
    return oms.fill(index, e.trade_id, e.last_qty, e.last_px, lite);
  } else if constexpr (std::is_same_v<E, model::OrderFillVoided>) {
    return oms.void_fill(index, e.trade_id, e.voided_qty, e.last_px);
  } else if constexpr (std::is_same_v<E, model::OrderUpdated>) {
    return oms.update(index, e.quantity, e.price);
  } else {
    return oms.apply(index, kind);
  }
}

} // namespace detail

// Applies `event` to the OMS; `index` is the order's slot when the order is known.
[[nodiscard]] inline EventOutcome apply_order_event(Oms& oms, const model::OrderEvent& event,
                                                    std::uint32_t& index) {
  index = oms.find(model::header_of(event).client_order_id);
  if (index == kNoIndex) {
    return EventOutcome::UnknownOrder;
  }
  OrderEventKind kind = OrderEventKind::Denied;
  if (!kind_of(event, kind)) {
    return EventOutcome::Refused; // OrderInitialized
  }
  const core::UnixNanos ts = model::header_of(event).ts_event;
  const bool fill = kind == OrderEventKind::Filled || kind == OrderEventKind::FillVoided;
  if (!fill && ts < oms.at(index).ts_venue) {
    return EventOutcome::Stale;
  }
  const std::uint32_t i = index;
  const core::Status s = std::visit(
      [&oms, i, kind](const auto& e) noexcept {
        const core::Status applied = detail::apply_to(oms, i, kind, e);
        if (core::ok(applied)) {
          detail::note_venue_order_id(oms.at(i), e);
        }
        return applied;
      },
      event);
  if (s == core::Status::DuplicateFill) {
    return EventOutcome::DuplicateFill;
  }
  if (!core::ok(s)) {
    return EventOutcome::Refused;
  }
  OrderRecord& r = oms.at(index);
  r.ts_venue = ts > r.ts_venue ? ts : r.ts_venue;
  return EventOutcome::Applied;
}

} // namespace jarvis::execution
