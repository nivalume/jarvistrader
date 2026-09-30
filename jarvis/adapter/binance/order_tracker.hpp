#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jarvis/adapter/binance/user_stream.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/reports.hpp"

// This node's orders at the venue (docs/architecture.md sections 8.2 and 8.3): turns the
// WebSocket API's answers and the user data stream's reports into kernel order events, with
// the event header the simulated exchange would give them.
//
//   order.place answered            OrderAccepted (unless x=NEW came first; once per order)
//   order.place / modify / cancel   OrderRejected (due_post_only for -5022), OrderModifyRejected,
//   refused                         OrderCancelRejected; reason "BINANCE_<code> <message>"
//   x=NEW                           OrderAccepted (once)
//   x=TRADE, x=CALCULATED           OrderFilled; CALCULATED marks a liquidation
//   x=CANCELED / EXPIRED            OrderCanceled / OrderExpired (once)
//   x=AMENDMENT                     OrderUpdated with the new quantity and price
//   TRADE_LITE                      OrderFilled flagged Lite, without commission
//
// Fills are deduplicated by (order id, trade id): the first report of a trade is the fill; when
// it was TRADE_LITE, the ORDER_TRADE_UPDATE of the same trade is passed on once more with its
// commission, which the kernel books without counting the fill twice (Oms::take_pending_
// commission); any further report is dropped. Reports older than the order's last update
// (o.T) are dropped. Reports of orders this node did not send are counted and dropped
// (reconciliation adopts or cancels them, section 15). ACCOUNT_UPDATE is merged into the full
// balance table, which goes to the kernel as an AccountState (total = wallet balance).

namespace jarvis::adapter::binance {

struct VenueIdentity {
  model::TraderId trader_id;
  model::AccountId account_id;
  std::vector<model::StrategyId> strategies; // by strategy index
  std::uint64_t seed = 0;                    // event ids derive from it and a counter
};

// The WebSocket API's answer to order.place.
struct PlaceAck {
  std::string client_order_id;
  std::uint64_t order_id = 0;
  std::string status; // NEW, PARTIALLY_FILLED, FILLED, EXPIRED, ...
  std::uint64_t update_time_ms = 0;
};

enum class RequestKind : std::uint8_t { Place, Modify, Cancel };

// The venue refused a request (a WebSocket API error, or an HTTP error of the REST fallback).
struct RequestError {
  RequestKind kind = RequestKind::Place;
  std::string client_order_id;
  int code = 0;
  std::string message;
  std::uint64_t time_ms = 0;
};

// An order the adapter sent and has not seen close (what a reconciliation snapshot asks about).
struct TrackedOrder {
  std::string symbol; // venue symbol
  std::string client_order_id;
  std::uint64_t venue_order_id = 0; // 0 when not acknowledged
};

// An order an earlier run of this node left open, restored by a resumed run (node/recovery.hpp):
// the tracker takes it as its own, so that the venue's reports of it are passed on and a
// reconciliation snapshot asks about it.
struct RecoveredOrder {
  std::uint16_t strategy = 0;
  model::InstrumentId instrument_id;
  model::ClientOrderId client_order_id;
  model::OrderSide side = model::OrderSide::Buy;
  model::OrderType type = model::OrderType::Limit;
  std::string venue_order_id; // empty when not acknowledged
  bool accepted = false;
  std::uint64_t last_update_ms = 0;
  std::vector<std::uint64_t> trades;      // trade ids already applied
  std::vector<std::uint64_t> lite_trades; // of those, Lite fills whose commission is still due
};

struct TrackerStats {
  std::uint64_t unknown_orders = 0;   // reports of orders not sent by this node
  std::uint64_t stale_reports = 0;    // older than the order's last update
  std::uint64_t duplicate_trades = 0; // a trade reported again after its full report
  std::uint64_t lite_fills = 0;
  std::uint64_t late_commissions = 0; // full reports passed on after a Lite fill
  std::uint64_t ignored = 0;          // reports that change nothing (a second NEW, ...)
  std::uint64_t account_updates = 0;
};

class OrderTracker {
public:
  OrderTracker(const SymbolTable& symbols, VenueIdentity identity)
      : symbols_{&symbols}, id_{std::move(identity)}, rng_{id_.seed} {}

  // A command the adapter is sending.
  void on_submit(const model::SubmitOrder& c);
  // An order of an earlier run (before any report arrives).
  void restore(const RecoveredOrder& r);

  [[nodiscard]] core::Status on_place_ack(const PlaceAck& a, core::UnixNanos recv,
                                          EventEmitter& out);
  [[nodiscard]] core::Status on_request_error(const RequestError& e, core::UnixNanos recv,
                                              EventEmitter& out);
  // A command the adapter could not send (order entry down, unknown instrument or order): the
  // same refusal events, built from the command when the tracker does not know the order.
  [[nodiscard]] core::Status on_local_refusal(const RequestError& e, std::uint16_t strategy,
                                              const model::InstrumentId& instrument,
                                              core::UnixNanos recv, EventEmitter& out);
  [[nodiscard]] core::Status on_report(const UserReport& r, core::UnixNanos recv,
                                       EventEmitter& out);

  // Reconciliation: the orders not seen closing, and the next trade id to ask for by venue
  // symbol (the ones seen, plus one). After the kernel reconciled a snapshot, absorb() takes
  // what it showed: venue ids, acceptance, closed orders and trades, so later reports of them are
  // passed on as the tracker would have had it seen them.
  [[nodiscard]] std::vector<TrackedOrder> unclosed() const;
  // The side of an order this node sent (a modify needs it).
  [[nodiscard]] std::optional<model::OrderSide> side_of(std::string_view client_order_id) const;
  [[nodiscard]] std::map<std::string, std::uint64_t> next_trades() const;
  void absorb(const model::VenueSnapshot& snapshot);

  [[nodiscard]] const TrackerStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t orders() const noexcept { return orders_.size(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
  enum class TradeState : std::uint8_t { Lite, Full };

  struct Order {
    std::uint16_t strategy = 0;
    std::uint32_t symbol = 0;
    model::InstrumentId instrument_id;
    model::ClientOrderId client_order_id;
    model::OrderSide side = model::OrderSide::Buy;
    model::OrderType type = model::OrderType::Limit;
    std::string venue_order_id;
    bool accepted = false;
    bool closed = false;
    std::uint64_t last_update_ms = 0;
    std::map<std::uint64_t, TradeState> trades;
  };

  [[nodiscard]] model::OrderEventHeader header(const Order& o, std::uint64_t ts_ms,
                                               core::UnixNanos recv);
  [[nodiscard]] Order* find(std::string_view client_order_id);
  core::Status accepted(Order& o, std::uint64_t ts_ms, core::UnixNanos recv, EventEmitter& out);
  core::Status fill(Order& o, std::uint64_t trade_id, std::string_view qty, std::string_view px,
                    bool maker, bool lite, bool liquidation, std::string_view commission,
                    std::string_view commission_asset, std::uint64_t ts_ms, core::UnixNanos recv,
                    EventEmitter& out);
  core::Status order_report(const OrderReport& r, core::UnixNanos recv, EventEmitter& out);
  core::Status refusal(Order& o, const RequestError& e, core::UnixNanos recv, EventEmitter& out);
  core::Status account_report(const AccountReport& r, core::UnixNanos recv, EventEmitter& out);
  core::Status fail(core::Status s, std::string what) {
    error_ = std::move(what);
    return s;
  }

  const SymbolTable* symbols_;
  VenueIdentity id_;
  core::CounterRng rng_;
  std::uint64_t serial_ = 0;
  std::unordered_map<std::string, Order> orders_;
  std::map<std::string, std::string> wallet_;   // asset -> wallet balance, all assets seen
  std::vector<model::AccountBalance> balances_; // what the last AccountState borrows
  TrackerStats stats_;
  std::string error_;
};

} // namespace jarvis::adapter::binance
