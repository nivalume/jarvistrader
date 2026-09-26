#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::model {

// The 17 nautilus order events (docs/architecture.md section 6.7). Field names follow nautilus;
// the fields every event shares live in `OrderEventHeader`.
//
// Not carried: OrderInitialized.linked_order_ids and .exec_algorithm_params (contingent orders
// and algorithm parameters live in kernel state), and the free-form `info` maps, which are
// replaced by `info_flags` on fills.

using ReasonText = core::FixedString<128>;
using TagsText = core::FixedString<64>;

struct OrderEventHeader {
  TraderId trader_id;
  StrategyId strategy_id;
  InstrumentId instrument_id;
  ClientOrderId client_order_id;
  Uuid4 event_id;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
  std::optional<Uuid4> causation_id;
  bool reconciliation = false;
};

struct OrderInitialized {
  OrderEventHeader header;
  OrderSide order_side = OrderSide::Buy;
  OrderType order_type = OrderType::Market;
  Quantity quantity;
  TimeInForce time_in_force = TimeInForce::Gtc;
  bool post_only = false;
  bool reduce_only = false;
  bool quote_quantity = false;
  std::optional<Price> price;
  std::optional<Price> activation_price;
  std::optional<Price> trigger_price;
  std::optional<TriggerType> trigger_type;
  std::optional<Decimal> limit_offset;
  std::optional<Decimal> trailing_offset;
  std::optional<TrailingOffsetType> trailing_offset_type;
  std::optional<core::UnixNanos> expire_time;
  std::optional<Quantity> display_qty;
  std::optional<TriggerType> emulation_trigger;
  std::optional<InstrumentId> trigger_instrument_id;
  std::optional<ContingencyType> contingency_type;
  std::optional<OrderListId> order_list_id;
  std::optional<ClientOrderId> parent_order_id;
  std::optional<ExecAlgorithmId> exec_algorithm_id;
  std::optional<ClientOrderId> exec_spawn_id;
  std::optional<TagsText> tags;
};

struct OrderDenied {
  OrderEventHeader header;
  ReasonText reason;
};

struct OrderEmulated {
  OrderEventHeader header;
};

struct OrderReleased {
  OrderEventHeader header;
  Price released_price;
};

struct OrderSubmitted {
  OrderEventHeader header;
  AccountId account_id;
};

struct OrderAccepted {
  OrderEventHeader header;
  VenueOrderId venue_order_id;
  AccountId account_id;
};

struct OrderRejected {
  OrderEventHeader header;
  AccountId account_id;
  ReasonText reason;
  bool due_post_only = false;
};

struct OrderCanceled {
  OrderEventHeader header;
  std::optional<VenueOrderId> venue_order_id;
  std::optional<AccountId> account_id;
  std::optional<ReasonText> reason;
};

struct OrderExpired {
  OrderEventHeader header;
  std::optional<VenueOrderId> venue_order_id;
  std::optional<AccountId> account_id;
};

struct OrderTriggered {
  OrderEventHeader header;
  std::optional<VenueOrderId> venue_order_id;
  std::optional<AccountId> account_id;
};

struct OrderPendingUpdate {
  OrderEventHeader header;
  AccountId account_id;
  std::optional<VenueOrderId> venue_order_id;
};

struct OrderPendingCancel {
  OrderEventHeader header;
  AccountId account_id;
  std::optional<VenueOrderId> venue_order_id;
};

struct OrderModifyRejected {
  OrderEventHeader header;
  ReasonText reason;
  std::optional<VenueOrderId> venue_order_id;
  std::optional<AccountId> account_id;
};

struct OrderCancelRejected {
  OrderEventHeader header;
  ReasonText reason;
  std::optional<VenueOrderId> venue_order_id;
  std::optional<AccountId> account_id;
};

struct OrderUpdated {
  OrderEventHeader header;
  std::optional<VenueOrderId> venue_order_id;
  std::optional<AccountId> account_id;
  Quantity quantity;
  std::optional<Price> price;
  std::optional<Price> trigger_price;
  std::optional<Price> protection_price;
  bool is_quote_quantity = false;
};

// Bits of OrderFilled::info_flags / OrderFillVoided::info_flags.
enum class FillInfo : std::uint8_t {
  Liquidation = 1U << 0U, // Binance x=CALCULATED: forced liquidation
  AutoDeleverage = 1U << 1U,
  Lite = 1U << 2U, // from TRADE_LITE; commission and realized PnL arrive later
};

struct OrderFilled {
  OrderEventHeader header;
  VenueOrderId venue_order_id;
  AccountId account_id;
  TradeId trade_id;
  OrderSide order_side = OrderSide::Buy;
  OrderType order_type = OrderType::Market;
  Quantity last_qty;
  Price last_px;
  Currency currency;
  LiquiditySide liquidity_side = LiquiditySide::NoLiquiditySide;
  std::optional<PositionId> position_id;
  std::optional<Money> commission;
  std::uint8_t info_flags = 0;
};

struct OrderFillVoided {
  OrderEventHeader header;
  VenueOrderId venue_order_id;
  AccountId account_id;
  TradeId correction_id;
  TradeId trade_id;
  Quantity voided_qty;
  std::optional<Money> commission_voided;
  OrderSide order_side = OrderSide::Buy;
  OrderType order_type = OrderType::Market;
  Price last_px;
  Currency currency;
  LiquiditySide liquidity_side = LiquiditySide::NoLiquiditySide;
  std::optional<PositionId> position_id;
  std::optional<ReasonText> reason;
  bool is_reopened = false;
  std::uint8_t info_flags = 0;
};

using OrderEvent =
    std::variant<OrderInitialized, OrderDenied, OrderEmulated, OrderReleased, OrderSubmitted,
                 OrderAccepted, OrderRejected, OrderCanceled, OrderExpired, OrderTriggered,
                 OrderPendingUpdate, OrderPendingCancel, OrderModifyRejected, OrderCancelRejected,
                 OrderUpdated, OrderFilled, OrderFillVoided>;

// Not noexcept: std::visit may throw bad_variant_access, which cannot happen for these types.
[[nodiscard]] constexpr const OrderEventHeader& header_of(const OrderEvent& event) {
  return std::visit([](const auto& e) -> const OrderEventHeader& { return e.header; }, event);
}

} // namespace jarvis::model
