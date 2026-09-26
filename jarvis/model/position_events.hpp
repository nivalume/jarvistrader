#pragma once

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

// nautilus position events. Where nautilus uses f64 (signed_qty, avg_px_open, avg_px_close,
// realized_return), jarvis keeps fixed-point integers: Decimal for the signed quantity and the
// return, Price for average prices (docs/architecture.md section 6.8).

struct PositionEventHeader {
  TraderId trader_id;
  StrategyId strategy_id;
  InstrumentId instrument_id;
  PositionId position_id;
  AccountId account_id;
  Uuid4 event_id;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
};

struct PositionOpened {
  PositionEventHeader header;
  ClientOrderId opening_order_id;
  OrderSide entry = OrderSide::Buy;
  PositionSide side = PositionSide::Flat;
  Decimal signed_qty;
  Quantity quantity;
  Quantity last_qty;
  Price last_px;
  Currency currency;
  Price avg_px_open;
  std::optional<Money> realized_pnl;
};

struct PositionChanged {
  PositionEventHeader header;
  ClientOrderId opening_order_id;
  OrderSide entry = OrderSide::Buy;
  PositionSide side = PositionSide::Flat;
  Decimal signed_qty;
  Quantity quantity;
  Quantity peak_quantity;
  Quantity last_qty;
  Price last_px;
  Currency currency;
  Price avg_px_open;
  std::optional<Price> avg_px_close;
  Decimal realized_return;
  std::optional<Money> realized_pnl;
  Money unrealized_pnl;
  core::UnixNanos ts_opened;
};

struct PositionClosed {
  PositionEventHeader header;
  ClientOrderId opening_order_id;
  std::optional<ClientOrderId> closing_order_id;
  OrderSide entry = OrderSide::Buy;
  PositionSide side = PositionSide::Flat;
  Decimal signed_qty;
  Quantity quantity;
  Quantity peak_quantity;
  Quantity last_qty;
  Price last_px;
  Currency currency;
  Price avg_px_open;
  std::optional<Price> avg_px_close;
  Decimal realized_return;
  std::optional<Money> realized_pnl;
  Money unrealized_pnl;
  core::DurationNanos duration;
  core::UnixNanos ts_opened;
  std::optional<core::UnixNanos> ts_closed;
};

struct PositionAdjusted {
  PositionEventHeader header;
  PositionAdjustmentType adjustment_type = PositionAdjustmentType::Commission;
  std::optional<Decimal> quantity_change;
  std::optional<Money> pnl_change;
  std::optional<core::FixedString<32>> reason;
};

using PositionEvent =
    std::variant<PositionOpened, PositionChanged, PositionClosed, PositionAdjusted>;

} // namespace jarvis::model
