#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "jarvis/adapter/binance/order_tracker.hpp"
#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/reports.hpp"

// The venue's view of the account for reconciliation (docs/architecture.md section 15.2), from
// the USDⓈ-M REST API. The kernel sets its state from what this returns and applies the held
// user stream events newer than T_s, so the snapshot has to be consistent:
//
//   1. T_s is the venue's clock before the first call, and the user stream is subscribed
//      before it (the caller's part);
//   2. openOrders, then GET /fapi/v1/order for every order the adapter has not seen close and
//      openOrders does not list (the venue not knowing one is left for the kernel to call lost);
//   3. userTrades per symbol, then balances and positions, then userTrades again: when new
//      trades turned up, balances and positions are read again, so every trade a position or
//      balance reflects is among the fill reports and none after them (the kernel sets the
//      position after applying the fills; a trade missing from the reports would be lost, one
//      the position does not reflect counted twice).
//
// Fills of an order the node placed name its ClientOrderId when the adapter or the order reports
// know its venue order id. Symbols and assets the node does not know are counted, not reported.

namespace jarvis::adapter::binance {

struct AccountSnapshotRequest {
  model::AccountId account_id;
  std::vector<std::string> symbols; // the node's instruments (venue symbols)
  std::vector<TrackedOrder> orders;
  // The next trade id to fetch, by symbol; symbols without one fetch from `trades_since_ms`
  // (the node's start: earlier trades belong to no order it knows).
  std::map<std::string, std::uint64_t> next_trade;
  std::int64_t trades_since_ms = 0;
  int trade_page = 1000;
  std::uint32_t max_pages = 20; // per symbol and read
  std::uint32_t max_rounds = 3; // re-reads of balances and positions before giving up
  std::uint64_t seed = 0;       // report ids derive from it
};

// A VenueSnapshot with its own storage.
struct AccountSnapshot {
  model::AccountId account_id;
  core::UnixNanos ts_snapshot;
  std::vector<model::AccountBalance> balances;
  std::vector<model::OrderStatusReport> orders;
  std::vector<model::FillReport> fills;
  std::vector<model::PositionStatusReport> positions;
  std::map<std::string, std::uint64_t> next_trade; // by symbol, after the fills reported
  std::size_t skipped = 0;                         // entries of unknown symbols or assets
  std::uint32_t rounds = 0;                        // balance and position reads
  std::uint32_t requests = 0;

  // The kernel input; it borrows this object's storage.
  [[nodiscard]] model::VenueSnapshot event(core::UnixNanos ts_init) const;
};

[[nodiscard]] core::Status assemble_snapshot(RestClient& rest, const SymbolTable& symbols,
                                             const AccountSnapshotRequest& request,
                                             AccountSnapshot& out, std::string& error);

} // namespace jarvis::adapter::binance
