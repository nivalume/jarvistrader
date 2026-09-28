#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/reports.hpp"

// Decoders of the USDⓈ-M REST responses the adapter needs (docs/architecture.md section 14).
// Not on the hot path; they use simdjson's validating DOM parser.

namespace jarvis::adapter::binance {

// GET /fapi/v1/depth: {"lastUpdateId", "E", "T", "bids": [[p, q]...], "asks": [...]}, with
// prices and sizes exact at the instrument's precision.
[[nodiscard]] core::Status decode_depth_snapshot(std::string_view json, const SymbolEntry& symbol,
                                                 core::UnixNanos recv_ns, DepthSnapshot& out,
                                                 std::string& error);

// The WebSocket API's answer to a `depth` request: {"id", "status", "result": {the snapshot},
// "rateLimits"}. `id` receives the request id; a status other than 200 is InvalidArgument with
// the venue's error in `error`.
[[nodiscard]] core::Status decode_ws_depth_response(std::string_view json,
                                                    const SymbolEntry& symbol,
                                                    core::UnixNanos recv_ns, std::string& id,
                                                    DepthSnapshot& out, std::string& error);

// ---- reconciliation (snapshot.hpp) ----------------------------------------------------------
// Venue times are milliseconds turned into UnixNanos; values are exact at the instrument's
// precision (an average entry price keeps the precision it is written with). Entries of symbols
// the table does not know, and assets that are not built-in currencies, are counted in
// `skipped`. Report ids are left for the caller to assign.

struct ReportContext {
  const SymbolTable* symbols = nullptr;
  model::AccountId account_id;
  core::UnixNanos recv; // ts_init of the reports
};

// GET /fapi/v1/openOrders (a list) or GET /fapi/v1/order (one object). Binance's statuses map to
// OrderStatus (EXPIRED_IN_MATCH is EXPIRED), GTX to post-only GTC, a zero price to none.
[[nodiscard]] core::Status decode_order_reports(std::string_view json, const ReportContext& ctx,
                                                std::vector<model::OrderStatusReport>& out,
                                                std::size_t& skipped, std::string& error);

// GET /fapi/v3/balance: total is the wallet balance, free the available balance (at most the
// total), locked the rest.
[[nodiscard]] core::Status decode_balances(std::string_view json,
                                           std::vector<model::AccountBalance>& out,
                                           std::size_t& skipped, std::string& error);

// GET /fapi/v3/positionRisk (one-way mode): the open positions; flat entries are left out.
[[nodiscard]] core::Status decode_positions(std::string_view json, const ReportContext& ctx,
                                            std::vector<model::PositionStatusReport>& out,
                                            std::size_t& skipped, std::string& error);

// GET /fapi/v1/userTrades: one FillReport per trade (commission positive when paid). `last_id`
// becomes the largest trade id of the page and `count` the number of trades in it.
[[nodiscard]] core::Status decode_user_trades(std::string_view json, const ReportContext& ctx,
                                              std::vector<model::FillReport>& out,
                                              std::uint64_t& last_id, std::size_t& count,
                                              std::string& error);

} // namespace jarvis::adapter::binance
