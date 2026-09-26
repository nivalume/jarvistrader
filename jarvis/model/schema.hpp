#pragma once

#include <string_view>

#include "jarvis/core/clock.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/position_events.hpp"

// Field descriptors of the model structs: `fields(value, f)` calls `f(name, field)` for every
// field in a fixed order. One descriptor per type serves the event log encoder and decoder
// (model/wire.hpp), the text rendering (node/event_text), and the Python bindings, so field
// names and order cannot drift between them. Names follow nautilus_trader.
//
// Event headers are flattened into the event, as nautilus exposes them. Nested value structs
// (BarType, BarSpecification, BookOrder) stay nested. Variable-length parts (the deltas of
// OrderBookDeltas, the balances and margins of AccountState) have no descriptor; the wire format
// and the bindings handle them explicitly.

namespace jarvis::model {

// NOLINTBEGIN(readability-function-cognitive-complexity)

template <typename F> constexpr void fields(OrderEventHeader& h, F&& f) {
  f("trader_id", h.trader_id), f("strategy_id", h.strategy_id), f("instrument_id", h.instrument_id),
      f("client_order_id", h.client_order_id), f("event_id", h.event_id), f("ts_event", h.ts_event),
      f("ts_init", h.ts_init), f("causation_id", h.causation_id),
      f("reconciliation", h.reconciliation);
}
template <typename F> constexpr void fields(PositionEventHeader& h, F&& f) {
  f("trader_id", h.trader_id), f("strategy_id", h.strategy_id), f("instrument_id", h.instrument_id),
      f("position_id", h.position_id), f("account_id", h.account_id), f("event_id", h.event_id),
      f("ts_event", h.ts_event), f("ts_init", h.ts_init);
}

// ---- market data ---------------------------------------------------------------------------

template <typename F> constexpr void fields(TradeTick& e, F&& f) {
  f("instrument_id", e.instrument_id), f("price", e.price), f("size", e.size),
      f("aggressor_side", e.aggressor_side), f("trade_id", e.trade_id), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(QuoteTick& e, F&& f) {
  f("instrument_id", e.instrument_id), f("bid_price", e.bid_price), f("ask_price", e.ask_price),
      f("bid_size", e.bid_size), f("ask_size", e.ask_size), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(BookOrder& e, F&& f) {
  f("side", e.side), f("price", e.price), f("size", e.size), f("order_id", e.order_id);
}
template <typename F> constexpr void fields(OrderBookDelta& e, F&& f) {
  f("instrument_id", e.instrument_id), f("action", e.action), f("order", e.order),
      f("flags", e.flags), f("sequence", e.sequence), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(OrderBookDepth& e, F&& f) {
  f("instrument_id", e.instrument_id), f("bids", e.bids), f("asks", e.asks),
      f("bid_counts", e.bid_counts), f("ask_counts", e.ask_counts), f("bid_levels", e.bid_levels),
      f("ask_levels", e.ask_levels), f("flags", e.flags), f("sequence", e.sequence),
      f("ts_event", e.ts_event), f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(BarSpecification& e, F&& f) {
  f("step", e.step), f("aggregation", e.aggregation), f("price_type", e.price_type);
}
template <typename F> constexpr void fields(BarType& e, F&& f) {
  f("instrument_id", e.instrument_id), f("spec", e.spec),
      f("aggregation_source", e.aggregation_source), f("composite", e.composite),
      f("composite_step", e.composite_step), f("composite_aggregation", e.composite_aggregation),
      f("composite_aggregation_source", e.composite_aggregation_source);
}
template <typename F> constexpr void fields(Bar& e, F&& f) {
  f("bar_type", e.bar_type), f("open", e.open), f("high", e.high), f("low", e.low),
      f("close", e.close), f("volume", e.volume), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(MarkPriceUpdate& e, F&& f) {
  f("instrument_id", e.instrument_id), f("value", e.value), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(IndexPriceUpdate& e, F&& f) {
  f("instrument_id", e.instrument_id), f("value", e.value), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(FundingRateUpdate& e, F&& f) {
  f("instrument_id", e.instrument_id), f("rate", e.rate), f("interval", e.interval),
      f("next_funding_ns", e.next_funding_ns), f("ts_event", e.ts_event), f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(InstrumentStatus& e, F&& f) {
  f("instrument_id", e.instrument_id), f("action", e.action), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init), f("reason", e.reason), f("trading_event", e.trading_event),
      f("is_trading", e.is_trading), f("is_quoting", e.is_quoting),
      f("is_short_sell_restricted", e.is_short_sell_restricted);
}
template <typename F> constexpr void fields(InstrumentClose& e, F&& f) {
  f("instrument_id", e.instrument_id), f("close_price", e.close_price),
      f("close_type", e.close_type), f("ts_event", e.ts_event), f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(LiquidationOrder& e, F&& f) {
  f("instrument_id", e.instrument_id), f("side", e.side), f("price", e.price),
      f("quantity", e.quantity), f("average_price", e.average_price),
      f("filled_quantity", e.filled_quantity), f("ts_event", e.ts_event), f("ts_init", e.ts_init);
}

// ---- instruments and accounts --------------------------------------------------------------

template <typename F> constexpr void fields(InstrumentCommon& c, F&& f) {
  f("id", c.id), f("raw_symbol", c.raw_symbol), f("base_currency", c.base_currency),
      f("quote_currency", c.quote_currency), f("settlement_currency", c.settlement_currency),
      f("is_inverse", c.is_inverse), f("price_precision", c.price_precision),
      f("size_precision", c.size_precision), f("price_increment", c.price_increment),
      f("size_increment", c.size_increment), f("multiplier", c.multiplier),
      f("lot_size", c.lot_size), f("margin_init", c.margin_init), f("margin_maint", c.margin_maint),
      f("max_quantity", c.max_quantity), f("min_quantity", c.min_quantity),
      f("max_notional", c.max_notional), f("min_notional", c.min_notional),
      f("max_price", c.max_price), f("min_price", c.min_price), f("ts_event", c.ts_event),
      f("ts_init", c.ts_init);
}
template <typename F> constexpr void fields(CurrencyPair& e, F&& f) { fields(e.common, f); }
template <typename F> constexpr void fields(CryptoPerpetual& e, F&& f) { fields(e.common, f); }
template <typename F> constexpr void fields(CryptoFuture& e, F&& f) {
  fields(e.common, f), f("underlying", e.underlying), f("activation_ns", e.activation_ns),
      f("expiration_ns", e.expiration_ns);
}
template <typename F> constexpr void fields(AccountBalance& e, F&& f) {
  f("total", e.total), f("locked", e.locked), f("free", e.free);
}
template <typename F> constexpr void fields(MarginBalance& e, F&& f) {
  f("initial", e.initial), f("maintenance", e.maintenance), f("currency", e.currency),
      f("instrument_id", e.instrument_id);
}

// ---- order events --------------------------------------------------------------------------

template <typename F> constexpr void fields(OrderInitialized& e, F&& f) {
  fields(e.header, f), f("order_side", e.order_side), f("order_type", e.order_type),
      f("quantity", e.quantity), f("time_in_force", e.time_in_force), f("post_only", e.post_only),
      f("reduce_only", e.reduce_only), f("quote_quantity", e.quote_quantity), f("price", e.price),
      f("activation_price", e.activation_price), f("trigger_price", e.trigger_price),
      f("trigger_type", e.trigger_type), f("limit_offset", e.limit_offset),
      f("trailing_offset", e.trailing_offset), f("trailing_offset_type", e.trailing_offset_type),
      f("expire_time", e.expire_time), f("display_qty", e.display_qty),
      f("emulation_trigger", e.emulation_trigger),
      f("trigger_instrument_id", e.trigger_instrument_id),
      f("contingency_type", e.contingency_type), f("order_list_id", e.order_list_id),
      f("parent_order_id", e.parent_order_id), f("exec_algorithm_id", e.exec_algorithm_id),
      f("exec_spawn_id", e.exec_spawn_id), f("tags", e.tags);
}
template <typename F> constexpr void fields(OrderDenied& e, F&& f) {
  fields(e.header, f), f("reason", e.reason);
}
template <typename F> constexpr void fields(OrderEmulated& e, F&& f) { fields(e.header, f); }
template <typename F> constexpr void fields(OrderReleased& e, F&& f) {
  fields(e.header, f), f("released_price", e.released_price);
}
template <typename F> constexpr void fields(OrderSubmitted& e, F&& f) {
  fields(e.header, f), f("account_id", e.account_id);
}
template <typename F> constexpr void fields(OrderAccepted& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id);
}
template <typename F> constexpr void fields(OrderRejected& e, F&& f) {
  fields(e.header, f), f("account_id", e.account_id), f("reason", e.reason),
      f("due_post_only", e.due_post_only);
}
template <typename F> constexpr void fields(OrderCanceled& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id),
      f("reason", e.reason);
}
template <typename F> constexpr void fields(OrderExpired& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id);
}
template <typename F> constexpr void fields(OrderTriggered& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id);
}
template <typename F> constexpr void fields(OrderPendingUpdate& e, F&& f) {
  fields(e.header, f), f("account_id", e.account_id), f("venue_order_id", e.venue_order_id);
}
template <typename F> constexpr void fields(OrderPendingCancel& e, F&& f) {
  fields(e.header, f), f("account_id", e.account_id), f("venue_order_id", e.venue_order_id);
}
template <typename F> constexpr void fields(OrderModifyRejected& e, F&& f) {
  fields(e.header, f), f("reason", e.reason), f("venue_order_id", e.venue_order_id),
      f("account_id", e.account_id);
}
template <typename F> constexpr void fields(OrderCancelRejected& e, F&& f) {
  fields(e.header, f), f("reason", e.reason), f("venue_order_id", e.venue_order_id),
      f("account_id", e.account_id);
}
template <typename F> constexpr void fields(OrderUpdated& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id),
      f("quantity", e.quantity), f("price", e.price), f("trigger_price", e.trigger_price),
      f("protection_price", e.protection_price), f("is_quote_quantity", e.is_quote_quantity);
}
template <typename F> constexpr void fields(OrderFilled& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id),
      f("trade_id", e.trade_id), f("order_side", e.order_side), f("order_type", e.order_type),
      f("last_qty", e.last_qty), f("last_px", e.last_px), f("currency", e.currency),
      f("liquidity_side", e.liquidity_side), f("position_id", e.position_id),
      f("commission", e.commission), f("info_flags", e.info_flags);
}
template <typename F> constexpr void fields(OrderFillVoided& e, F&& f) {
  fields(e.header, f), f("venue_order_id", e.venue_order_id), f("account_id", e.account_id),
      f("correction_id", e.correction_id), f("trade_id", e.trade_id), f("voided_qty", e.voided_qty),
      f("commission_voided", e.commission_voided), f("order_side", e.order_side),
      f("order_type", e.order_type), f("last_px", e.last_px), f("currency", e.currency),
      f("liquidity_side", e.liquidity_side), f("position_id", e.position_id), f("reason", e.reason),
      f("is_reopened", e.is_reopened), f("info_flags", e.info_flags);
}

// ---- position events -----------------------------------------------------------------------

template <typename F> constexpr void fields(PositionOpened& e, F&& f) {
  fields(e.header, f), f("opening_order_id", e.opening_order_id), f("entry", e.entry),
      f("side", e.side), f("signed_qty", e.signed_qty), f("quantity", e.quantity),
      f("last_qty", e.last_qty), f("last_px", e.last_px), f("currency", e.currency),
      f("avg_px_open", e.avg_px_open), f("realized_pnl", e.realized_pnl);
}
template <typename F> constexpr void fields(PositionChanged& e, F&& f) {
  fields(e.header, f), f("opening_order_id", e.opening_order_id), f("entry", e.entry),
      f("side", e.side), f("signed_qty", e.signed_qty), f("quantity", e.quantity),
      f("peak_quantity", e.peak_quantity), f("last_qty", e.last_qty), f("last_px", e.last_px),
      f("currency", e.currency), f("avg_px_open", e.avg_px_open), f("avg_px_close", e.avg_px_close),
      f("realized_return", e.realized_return), f("realized_pnl", e.realized_pnl),
      f("unrealized_pnl", e.unrealized_pnl), f("ts_opened", e.ts_opened);
}
template <typename F> constexpr void fields(PositionClosed& e, F&& f) {
  fields(e.header, f), f("opening_order_id", e.opening_order_id),
      f("closing_order_id", e.closing_order_id), f("entry", e.entry), f("side", e.side),
      f("signed_qty", e.signed_qty), f("quantity", e.quantity), f("peak_quantity", e.peak_quantity),
      f("last_qty", e.last_qty), f("last_px", e.last_px), f("currency", e.currency),
      f("avg_px_open", e.avg_px_open), f("avg_px_close", e.avg_px_close),
      f("realized_return", e.realized_return), f("realized_pnl", e.realized_pnl),
      f("unrealized_pnl", e.unrealized_pnl), f("duration", e.duration), f("ts_opened", e.ts_opened),
      f("ts_closed", e.ts_closed);
}
template <typename F> constexpr void fields(PositionAdjusted& e, F&& f) {
  fields(e.header, f), f("adjustment_type", e.adjustment_type),
      f("quantity_change", e.quantity_change), f("pnl_change", e.pnl_change), f("reason", e.reason);
}

// ---- kernel events -------------------------------------------------------------------------

template <typename F> constexpr void fields(TimerFired& e, F&& f) {
  f("owner", e.key.owner), f("id", e.key.id), f("deadline", e.deadline), f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(BatchEnd& e, F&& f) {
  f("batch", e.batch), f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(NodeLifecycle& e, F&& f) {
  f("from", e.from), f("to", e.to), f("reason", e.reason), f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(StrategyError& e, F&& f) {
  f("strategy_index", e.strategy_index), f("kind", e.kind), f("message_hash", e.message_hash),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(Shutdown& e, F&& f) {
  f("mode", e.mode), f("ts_init", e.ts_init);
}

// ---- kernel outputs ------------------------------------------------------------------------

template <typename F> constexpr void fields(FeatureUpdate& e, F&& f) {
  f("feature_id", e.feature_id), f("value", e.value), f("ts_event", e.ts_event),
      f("ts_init", e.ts_init);
}
template <typename F> constexpr void fields(StrategyRecord& e, F&& f) {
  f("strategy_index", e.strategy_index), f("tag", e.tag), f("value", e.value),
      f("ts_init", e.ts_init);
}

// NOLINTEND(readability-function-cognitive-complexity)

// True for types with a field descriptor.
template <typename T>
concept Described =
    requires(T& value) { fields(value, [](std::string_view /*name*/, auto& /*field*/) {}); };

} // namespace jarvis::model
