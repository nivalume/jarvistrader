#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/uuid.hpp"

// The venue's view of an account for reconciliation (docs/architecture.md section 15). The three
// reports follow nautilus_trader's OrderStatusReport, FillReport and PositionStatusReport with
// the fields reconciliation uses; VenueSnapshot plays the part of its ExecutionMassStatus, with
// the balances and the venue time the snapshot reflects (T_s) added.
//
// Left out from nautilus: OrderStatusReport's order_list_id, contingency_type, expire_time,
// trigger_price, trigger_type, limit_offset, trailing_offset(_type), avg_px, display_qty,
// cancel_reason, ts_triggered; FillReport's venue_position_id; PositionStatusReport's
// venue_position_id and avg_px_open (netting accounts: one position per instrument).

namespace jarvis::model {

// What a ReconciliationDiff compares (docs/architecture.md section 15.2, step 5).
enum class ReconcileDiffKind : std::uint8_t {
  Position = 0,       // the venue position (signed raw) against the local one
  Balance = 1,        // a wallet balance
  FilledQuantity = 2, // an order's filled quantity, before the missed fills were applied
  LostOrder = 3,      // a local open order the venue does not know (closed as lost)
  ExternalOrder = 4,  // a venue order the node did not place
};

[[nodiscard]] constexpr std::string_view to_string(ReconcileDiffKind v) noexcept {
  switch (v) {
  case ReconcileDiffKind::Position:
    return "POSITION";
  case ReconcileDiffKind::Balance:
    return "BALANCE";
  case ReconcileDiffKind::FilledQuantity:
    return "FILLED_QUANTITY";
  case ReconcileDiffKind::LostOrder:
    return "LOST_ORDER";
  case ReconcileDiffKind::ExternalOrder:
    return "EXTERNAL_ORDER";
  }
  return "?";
}

[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, ReconcileDiffKind& out) noexcept {
  if (v > static_cast<std::uint8_t>(ReconcileDiffKind::ExternalOrder)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<ReconcileDiffKind>(v);
  return core::Status::Ok;
}

struct OrderStatusReport {
  AccountId account_id;
  InstrumentId instrument_id;
  std::optional<ClientOrderId> client_order_id; // absent for an order placed elsewhere
  VenueOrderId venue_order_id;
  OrderSide order_side = OrderSide::Buy;
  OrderType order_type = OrderType::Limit;
  TimeInForce time_in_force = TimeInForce::Gtc;
  OrderStatus order_status = OrderStatus::Accepted;
  Quantity quantity;
  Quantity filled_qty;
  std::optional<Price> price;
  bool post_only = false;
  bool reduce_only = false;
  Uuid4 report_id;
  core::UnixNanos ts_accepted;
  core::UnixNanos ts_last; // the venue time of the order's last change
  core::UnixNanos ts_init;
};

struct FillReport {
  AccountId account_id;
  InstrumentId instrument_id;
  VenueOrderId venue_order_id;
  TradeId trade_id;
  OrderSide order_side = OrderSide::Buy;
  Quantity last_qty;
  Price last_px;
  Money commission;
  LiquiditySide liquidity_side = LiquiditySide::NoLiquiditySide;
  std::optional<ClientOrderId> client_order_id;
  Uuid4 report_id;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
};

struct PositionStatusReport {
  AccountId account_id;
  InstrumentId instrument_id;
  PositionSide position_side = PositionSide::Flat;
  Quantity quantity; // unsigned; the side says which way
  Uuid4 report_id;
  core::UnixNanos ts_last;
  core::UnixNanos ts_init;
};

// One snapshot of the venue at its time `ts_snapshot` (T_s): the balances, every open order and
// the final state of the node's orders that closed while it was not listening, the fills it
// may have missed, and the positions. The spans are borrowed like AccountState's.
struct VenueSnapshot {
  AccountId account_id;
  core::UnixNanos ts_snapshot;
  std::span<const AccountBalance> balances;
  std::span<const OrderStatusReport> orders;
  std::span<const FillReport> fills;
  std::span<const PositionStatusReport> positions;
  Uuid4 event_id;
  core::UnixNanos ts_init;
};

} // namespace jarvis::model
