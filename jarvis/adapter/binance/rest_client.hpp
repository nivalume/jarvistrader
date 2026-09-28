#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/adapter/binance/order_tracker.hpp"
#include "jarvis/adapter/binance/requests.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/network/https_client.hpp"
#include "jarvis/network/signer.hpp"

// The USDⓈ-M REST endpoints the adapter uses (docs/architecture.md sections 14.4 to 14.7): startup
// checks, the listenKey, depth snapshots and the order fallback when the WebSocket API is down.
// Blocking; one client per thread (startup, the order-sender). Every response's rate headers
// become RateLimitFeedback; 429 reports Retry-After; 418 (an IP ban) makes every call fail fast
// until the ban's Retry-After has passed.

namespace jarvis::adapter::binance {

struct RestConfig {
  std::string base_url = "https://fapi.binance.com";
  std::string api_key;
  network::Signer signer;
  std::int64_t recv_window_ms = 5000;
  network::TlsOptions tls;
  std::chrono::milliseconds timeout{10'000};
  std::function<std::int64_t()> now_ms; // local UTC milliseconds; system_clock when empty
};

struct RestResponse {
  int status = 0;
  std::string body;
  int error_code = 0; // the venue's code when the body is an error
  std::string error_msg;
  std::vector<model::RateLimitFeedback> limits;
  std::optional<std::int64_t> retry_after_s;
};

// What an API key may do (GET /sapi/v1/account/apiRestrictions, on api.binance.com).
struct KeyRestrictions {
  bool ip_restrict = false;
  bool enable_futures = false;
  bool enable_withdrawals = false;
  bool enable_reading = false;
};

// A symbol's position settings (GET /fapi/v2/positionRisk).
struct PositionRisk {
  std::string symbol;
  std::uint32_t leverage = 0;
  std::string margin_type; // "cross" or "isolated"
  std::string position_amount;
};

class RestClient {
public:
  explicit RestClient(RestConfig config);

  // IoError when the exchange failed; Ok with the HTTP status otherwise (the endpoint helpers
  // below turn statuses into results). InvalidState during a 418 ban.
  [[nodiscard]] core::Status call(std::string_view method, std::string_view path, Params params,
                                  Security security, RestResponse& out, std::string& error);

  // GET /fapi/v1/time; signed requests then carry the venue's clock.
  [[nodiscard]] core::Status sync_time(std::string& error);
  [[nodiscard]] std::int64_t time_offset_ms() const noexcept { return offset_ms_; }
  void set_time_offset(std::int64_t ms) noexcept { offset_ms_ = ms; } // measured elsewhere
  [[nodiscard]] bool banned() const;
  // Rate feedback of every response since the last call to this.
  [[nodiscard]] std::vector<model::RateLimitFeedback> take_limits();

  [[nodiscard]] core::Status position_mode(bool& dual, std::string& error);
  [[nodiscard]] core::Status multi_assets_mode(bool& on, std::string& error);
  [[nodiscard]] core::Status position_risk(std::string_view symbol, PositionRisk& out,
                                           std::string& error);
  [[nodiscard]] core::Status exchange_info(std::string& body, std::string& error);
  // On a client for the spot API host: the key's permissions.
  [[nodiscard]] core::Status key_restrictions(KeyRestrictions& out, std::string& error);
  [[nodiscard]] core::Status create_listen_key(std::string& key, std::string& error);
  [[nodiscard]] core::Status keep_alive_listen_key(std::string& error);
  [[nodiscard]] core::Status close_listen_key(std::string& error);
  [[nodiscard]] core::Status depth(std::string_view symbol, int limit, std::string& body,
                                   std::string& error);

  // Reconciliation (snapshot.hpp): the raw JSON bodies, decoded by rest_codec.hpp.
  [[nodiscard]] core::Status open_orders(std::string& body, std::string& error);
  // GET /fapi/v1/order by ClientOrderId; `found` is false when the venue does not know it
  // (-2013).
  [[nodiscard]] core::Status query_order(std::string_view symbol, std::string_view client_order_id,
                                         std::string& body, bool& found, std::string& error);
  [[nodiscard]] core::Status balances(std::string& body, std::string& error);
  [[nodiscard]] core::Status positions(std::string& body, std::string& error);
  // GET /fapi/v1/userTrades from trade id `from_id` when given, else from `start_ms`.
  [[nodiscard]] core::Status user_trades(std::string_view symbol,
                                         std::optional<std::uint64_t> from_id,
                                         std::int64_t start_ms, int limit, std::string& body,
                                         std::string& error);

  // The order fallback: Ok with the acknowledgement, or Ok with `refused` set and the venue's
  // refusal (an HTTP 4xx with a code); IoError when the outcome is unknown (in flight).
  [[nodiscard]] core::Status place(const Params& order, PlaceAck& ack, RequestError& refusal,
                                   bool& refused, std::string& error);
  [[nodiscard]] core::Status modify(const Params& order, PlaceAck& ack, RequestError& refusal,
                                    bool& refused, std::string& error);
  [[nodiscard]] core::Status cancel(const Params& order, PlaceAck& ack, RequestError& refusal,
                                    bool& refused, std::string& error);

  [[nodiscard]] std::int64_t now_ms() const; // local UTC milliseconds

private:
  [[nodiscard]] core::Status json_call(std::string_view method, std::string_view path,
                                       Params params, Security security, RestResponse& out,
                                       std::string& error);
  [[nodiscard]] core::Status order_call(std::string_view method, RequestKind kind,
                                        const Params& order, PlaceAck& ack, RequestError& refusal,
                                        bool& refused, std::string& error);

  RestConfig config_;
  network::HttpsClient http_;
  std::int64_t offset_ms_ = 0;
  std::int64_t banned_until_ms_ = 0;
  std::vector<model::RateLimitFeedback> limits_;
};

} // namespace jarvis::adapter::binance
