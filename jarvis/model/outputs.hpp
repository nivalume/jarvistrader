#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/reports.hpp"

// Kernel outputs (docs/architecture.md section 16.1): what `step` produces, written to the event
// log after the input that caused it. Replay recomputes them from the inputs and compares them
// byte for byte (ReplayDivergence).

namespace jarvis::model {

using FeatureId = std::uint32_t;

// A kernel feature value delivered to at least one subscriber.
struct FeatureUpdate {
  FeatureId feature_id = 0;
  Decimal value;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
};

using RecordTag = core::FixedString<32>;

// A value a strategy recorded with ctx.record(tag, value): its deterministic, replay-checked
// output when it does not trade.
struct StrategyRecord {
  std::uint16_t strategy_index = 0;
  RecordTag tag;
  Decimal value;
  core::UnixNanos ts_init;
};

// ---- commands to a venue (docs/architecture.md section 9.1) ------------------------------
// The engine emits a command after both risk gates passed; the node sends it (live: the order
// sender; backtest: the simulated exchange). The venue's answers come back as order events.

struct SubmitOrder {
  std::uint16_t strategy_index = 0;
  ClientOrderId client_order_id;
  InstrumentId instrument_id;
  OrderSide order_side = OrderSide::Buy;
  OrderType order_type = OrderType::Limit;
  Quantity quantity;
  std::optional<Price> price;
  TimeInForce time_in_force = TimeInForce::Gtc;
  bool post_only = false;
  bool reduce_only = false;
  std::optional<core::UnixNanos> expire_time;
  core::UnixNanos ts_init;
};

// Binance order.modify: a LIMIT order's new quantity and price.
struct ModifyOrder {
  std::uint16_t strategy_index = 0;
  ClientOrderId client_order_id;
  InstrumentId instrument_id;
  std::optional<VenueOrderId> venue_order_id;
  Quantity quantity;
  Price price;
  core::UnixNanos ts_init;
};

struct CancelOrder {
  std::uint16_t strategy_index = 0;
  ClientOrderId client_order_id;
  InstrumentId instrument_id;
  std::optional<VenueOrderId> venue_order_id;
  core::UnixNanos ts_init;
};

// Every open order of the instrument (the KillSwitch and ctx.cancel_all).
struct CancelAllOrders {
  std::uint16_t strategy_index = 0;
  InstrumentId instrument_id;
  core::UnixNanos ts_init;
};

// ---- reconciliation (docs/architecture.md section 15.2) ------------------------------------

// One difference between the local state and a VenueSnapshot, found while reconciling; the
// venue's value is adopted. Raw values: positions and quantities at the instrument's size
// precision (positions signed), balances at the currency's precision.
struct ReconciliationDiff {
  AccountId account_id;
  ReconcileDiffKind kind = ReconcileDiffKind::Position;
  std::optional<InstrumentId> instrument_id;
  std::optional<ClientOrderId> client_order_id;
  std::optional<Currency> currency;
  std::int64_t local_raw = 0;
  std::int64_t venue_raw = 0;
  core::UnixNanos ts_init;
};

// The end of one reconciliation: the account is synced as of the snapshot's T_s plus the
// buffered events applied after it.
struct ReconcileOutcome {
  AccountId account_id;
  core::UnixNanos ts_snapshot;
  std::uint32_t orders = 0;   // local open orders compared
  std::uint32_t fills = 0;    // missed fills applied from the snapshot
  std::uint32_t closed = 0;   // local orders the snapshot showed closed
  std::uint32_t lost = 0;     // local orders the venue does not know
  std::uint32_t external = 0; // venue orders the node did not place
  std::uint32_t diffs = 0;    // ReconciliationDiff outputs
  std::uint32_t buffered = 0; // buffered events applied after T_s
  core::UnixNanos ts_init;
};

// Kernel outputs: features, strategy records, venue commands, the orders the risk gates denied
// (a decision without a command, still checked by replay), and reconciliation results.
using Output = std::variant<FeatureUpdate, StrategyRecord, SubmitOrder, ModifyOrder, CancelOrder,
                            CancelAllOrders, OrderDenied, ReconciliationDiff, ReconcileOutcome>;

} // namespace jarvis::model
