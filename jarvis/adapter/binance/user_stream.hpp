#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "jarvis/core/status.hpp"

// Reports of the USDⓈ-M user data stream (docs/architecture.md section 14.5), parsed but not yet
// interpreted: numbers stay strings until the order tracker reads them at the instrument's
// precision. The stream is low-rate, so it goes through simdjson's validating DOM parser.

namespace jarvis::adapter::binance {

// ORDER_TRADE_UPDATE: the "o" object and the event's times.
struct OrderReport {
  std::string symbol;                    // s
  std::string client_order_id;           // c
  std::string side;                      // S: BUY, SELL
  std::string order_type;                // o
  std::string time_in_force;             // f
  std::string execution_type;            // x: NEW, TRADE, CANCELED, CALCULATED, EXPIRED, AMENDMENT
  std::string order_status;              // X
  std::uint64_t order_id = 0;            // i
  std::string orig_qty;                  // q
  std::string price;                     // p
  std::string last_qty;                  // l
  std::string last_price;                // L
  std::string cum_qty;                   // z
  std::string commission;                // n
  std::string commission_asset;          // N
  std::uint64_t trade_id = 0;            // t
  bool maker = false;                    // m
  std::string realized_profit;           // rp
  std::uint64_t order_time_ms = 0;       // o.T: the order's trade or update time
  std::uint64_t event_time_ms = 0;       // E
  std::uint64_t transaction_time_ms = 0; // T
};

// TRADE_LITE: a fill reported before ORDER_TRADE_UPDATE, without commission.
struct TradeLiteReport {
  std::string symbol;          // s
  std::string client_order_id; // c
  std::string side;            // S
  std::string last_qty;        // l
  std::string last_price;      // L
  std::uint64_t trade_id = 0;  // t
  std::uint64_t order_id = 0;  // i
  bool maker = false;          // m
  std::uint64_t event_time_ms = 0;
  std::uint64_t transaction_time_ms = 0;
};

struct BalanceReport {
  std::string asset;        // a
  std::string wallet;       // wb
  std::string cross_wallet; // cw
  std::string change;       // bc
};

struct PositionReport {
  std::string symbol;        // s
  std::string amount;        // pa
  std::string entry_price;   // ep
  std::string unrealized;    // up
  std::string margin_type;   // mt
  std::string position_side; // ps
};

// ACCOUNT_UPDATE: only the assets and positions that changed.
struct AccountReport {
  std::string reason; // m: ORDER, FUNDING_FEE, DEPOSIT, ...
  std::vector<BalanceReport> balances;
  std::vector<PositionReport> positions;
  std::uint64_t event_time_ms = 0;
  std::uint64_t transaction_time_ms = 0;
};

struct MarginCallReport {
  std::string cross_wallet; // cw
  std::vector<PositionReport> positions;
  std::uint64_t event_time_ms = 0;
};

// ACCOUNT_CONFIG_UPDATE: a symbol's leverage, or the multi-assets mode.
struct ConfigReport {
  std::string symbol;
  std::optional<std::uint32_t> leverage;
  std::optional<bool> multi_assets;
  std::uint64_t event_time_ms = 0;
};

struct ListenKeyExpiredReport {
  std::string listen_key;
  std::uint64_t event_time_ms = 0;
};

using UserReport = std::variant<OrderReport, TradeLiteReport, AccountReport, MarginCallReport,
                                ConfigReport, ListenKeyExpiredReport>;

// One message of the user data stream (a bare event, or the combined-stream envelope).
// UnsupportedMessage for an event type not listed above; ParseError for malformed JSON.
[[nodiscard]] core::Status decode_user_report(std::string_view json, UserReport& out,
                                              std::string& error);

} // namespace jarvis::adapter::binance
