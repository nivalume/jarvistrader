#pragma once

#include <cstdint>
#include <optional>

#include "jarvis/core/status.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"

// The state of one order under the apply-phase rules of specs/tla/OrderLifecycle.tla
// (docs/architecture.md section 8.1): the transition table decides the next status, then
//   - a fill makes the order FILLED when it completes it, keeps CANCELED after a cancel it
//     overtook, and otherwise makes it PARTIALLY_FILLED, except that a pending update or cancel
//     stays pending and remembers PARTIALLY_FILLED as the status to return to;
//   - MODIFY_REJECTED (unless a cancel is pending), CANCEL_REJECTED while a cancel is pending,
//     and UPDATED while a request is pending restore the previous status;
//   - the previous status is saved on every transition except the rejections and while a
//     request is already pending.
// Duplicate fills and voids of unknown trades are the OMS's to refuse (it owns the trade
// records); this class checks quantities and transitions.

namespace jarvis::execution {

class OrderState {
public:
  OrderState() = default;
  explicit OrderState(model::Quantity quantity) noexcept
      : quantity_{quantity}, filled_{zero_like(quantity)} {}

  // An order in a given state (snapshots, recovery and exhaustive tests). `filled` must not
  // exceed `quantity`; both carry the same precision.
  [[nodiscard]] static core::Status restore(OrderStatus status, std::optional<OrderStatus> previous,
                                            model::Quantity quantity, model::Quantity filled,
                                            OrderState& out) noexcept {
    if (filled.raw() > quantity.raw() || filled.precision() != quantity.precision()) {
      return core::Status::InvalidArgument;
    }
    out = OrderState{quantity};
    out.status_ = status;
    out.previous_ = previous;
    out.filled_ = filled;
    return core::Status::Ok;
  }

  [[nodiscard]] OrderStatus status() const noexcept { return status_; }
  [[nodiscard]] std::optional<OrderStatus> previous() const noexcept { return previous_; }
  [[nodiscard]] model::Quantity quantity() const noexcept { return quantity_; }
  [[nodiscard]] model::Quantity filled() const noexcept { return filled_; }
  [[nodiscard]] model::Quantity leaves() const noexcept {
    model::Quantity out;
    static_cast<void>(
        model::Quantity::from_raw(quantity_.raw() - filled_.raw(), quantity_.precision(), out));
    return out;
  }

  // Events without a quantity. Filled, FillVoided and Updated have their own methods.
  [[nodiscard]] core::Status apply(OrderEventKind kind) noexcept {
    if (kind == OrderEventKind::Filled || kind == OrderEventKind::FillVoided ||
        kind == OrderEventKind::Updated) {
      return core::Status::InvalidArgument;
    }
    OrderStatus to = status_;
    if (!next_status(status_, kind, to)) {
      return core::Status::InvalidTransition;
    }
    const bool restore =
        (kind == OrderEventKind::ModifyRejected && status_ != OrderStatus::PendingCancel) ||
        (kind == OrderEventKind::CancelRejected && status_ == OrderStatus::PendingCancel);
    if (restore && !previous_) {
      return core::Status::InvalidState;
    }
    const OrderStatus back = previous_.value_or(status_);
    save_previous(kind);
    status_ = restore ? back : to;
    return core::Status::Ok;
  }

  // OrderUpdated with the order's new quantity, which must exceed what is filled.
  [[nodiscard]] core::Status update(model::Quantity quantity) noexcept {
    OrderStatus to = status_;
    if (!next_status(status_, OrderEventKind::Updated, to)) {
      return core::Status::InvalidTransition;
    }
    if (quantity.raw() <= filled_.raw() || quantity.precision() != quantity_.precision()) {
      return core::Status::InvalidArgument;
    }
    const bool restore = is_pending(status_) && previous_.has_value();
    const OrderStatus back = previous_.value_or(status_);
    save_previous(OrderEventKind::Updated);
    status_ = restore ? back : to;
    quantity_ = quantity;
    return core::Status::Ok;
  }

  // OrderFilled for `qty`, at most the leaves quantity (the OMS has refused duplicates).
  [[nodiscard]] core::Status fill(model::Quantity qty) noexcept {
    OrderStatus to = status_;
    if (!next_status(status_, OrderEventKind::Filled, to)) {
      return core::Status::InvalidTransition;
    }
    if (qty.raw() == 0 || qty.raw() > quantity_.raw() - filled_.raw() ||
        qty.precision() != quantity_.precision()) {
      return core::Status::InvalidArgument;
    }
    const std::uint64_t filled = filled_.raw() + qty.raw();
    if (filled >= quantity_.raw()) {
      save_previous(OrderEventKind::Filled);
      status_ = OrderStatus::Filled;
    } else if (status_ == OrderStatus::Canceled) {
      save_previous(OrderEventKind::Filled);
    } else if (is_pending(status_)) {
      previous_ = OrderStatus::PartiallyFilled;
    } else {
      save_previous(OrderEventKind::Filled);
      status_ = OrderStatus::PartiallyFilled;
    }
    static_cast<void>(model::Quantity::from_raw(filled, quantity_.precision(), filled_));
    return core::Status::Ok;
  }

  // OrderFillVoided for `voided` of an earlier fill (the OMS checks the trade and its size).
  [[nodiscard]] core::Status void_fill(model::Quantity voided) noexcept {
    OrderStatus to = status_;
    if (!next_status(status_, OrderEventKind::FillVoided, to)) {
      return core::Status::InvalidTransition;
    }
    if (voided.raw() == 0 || voided.raw() > filled_.raw() ||
        voided.precision() != quantity_.precision()) {
      return core::Status::InvalidArgument;
    }
    const std::uint64_t filled = filled_.raw() - voided.raw();
    const OrderStatus source = status_;
    save_previous(OrderEventKind::FillVoided);
    if (source == OrderStatus::Filled) {
      status_ = OrderStatus::Voided;
    } else if (source == OrderStatus::Canceled || source == OrderStatus::Expired ||
               source == OrderStatus::Voided || source == OrderStatus::Triggered ||
               is_pending(source)) {
      status_ = source;
    } else {
      status_ = filled == 0 ? OrderStatus::Accepted : OrderStatus::PartiallyFilled;
    }
    static_cast<void>(model::Quantity::from_raw(filled, quantity_.precision(), filled_));
    return core::Status::Ok;
  }

private:
  static model::Quantity zero_like(model::Quantity q) noexcept {
    model::Quantity out;
    static_cast<void>(model::Quantity::from_raw(0, q.precision(), out));
    return out;
  }

  void save_previous(OrderEventKind kind) noexcept {
    if (kind != OrderEventKind::ModifyRejected && kind != OrderEventKind::CancelRejected &&
        !is_pending(status_)) {
      previous_ = status_;
    }
  }

  OrderStatus status_ = OrderStatus::Initialized;
  std::optional<OrderStatus> previous_;
  model::Quantity quantity_;
  model::Quantity filled_;
};

} // namespace jarvis::execution
