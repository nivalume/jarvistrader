#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jarvis/adapter/binance/order_tracker.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/network/signer.hpp"

// Requests to the USDⓈ-M trading endpoints and their answers (docs/architecture.md sections
// 14.4 and 14.7), without I/O: kernel commands become request parameters, parameters become a
// signed REST query or a WebSocket API request, and answers become acknowledgements, refusals
// and RateLimitFeedback events.
//
// Signing: REST signs the query string as sent (HMAC-SHA256 as hex, Ed25519 as base64, which
// is then percent-encoded); the WebSocket API signs its parameters sorted by name and joined
// as k=v&... (session.logon; requests after a logon are not signed).

namespace jarvis::adapter::binance {

using Params = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::string url_encode(std::string_view text);
[[nodiscard]] std::string query_string(const Params& params); // values percent-encoded

// The parameters of order.place / POST /fapi/v1/order for a kernel SubmitOrder. Post-only
// limits are GTX; the response type is ACK (fills come from the user data stream).
[[nodiscard]] core::Status place_params(const model::SubmitOrder& c, std::string_view symbol,
                                        Params& out, std::string& error);
// order.modify / PUT /fapi/v1/order: Binance needs the side, which the command does not carry.
[[nodiscard]] core::Status modify_params(const model::ModifyOrder& c, std::string_view symbol,
                                         model::OrderSide side, Params& out, std::string& error);
// order.cancel / DELETE /fapi/v1/order, by the client order id.
[[nodiscard]] core::Status cancel_params(const model::CancelOrder& c, std::string_view symbol,
                                         Params& out);

// A REST query with timestamp, recvWindow and signature appended: "k=v&...&signature=...".
[[nodiscard]] std::string signed_query(Params params, std::int64_t timestamp_ms,
                                       std::int64_t recv_window_ms, const network::Signer& signer);

// A WebSocket API request: {"id":..,"method":..,"params":{..}}. With a signer, apiKey,
// timestamp and signature are added (session.logon); otherwise only timestamp.
[[nodiscard]] std::string ws_request(std::string_view id, std::string_view method, Params params,
                                     std::int64_t timestamp_ms, const network::Signer* signer,
                                     std::string_view api_key = {});

// The WebSocket API's answer to one request.
struct WsAnswer {
  std::string id;
  int status = 0; // 200 on success
  int error_code = 0;
  std::string error_msg;
  std::optional<PlaceAck> ack; // an order result (orderId, clientOrderId, status, updateTime)
  std::vector<model::RateLimitFeedback> limits;
};

[[nodiscard]] core::Status decode_ws_answer(std::string_view json, core::UnixNanos recv,
                                            WsAnswer& out, std::string& error);

// {"code": -1121, "msg": "..."} of a REST error body; false when the body is not one.
[[nodiscard]] bool decode_rest_error(std::string_view json, int& code, std::string& msg);
// A REST order result ({"orderId", "clientOrderId", "status", "updateTime", ...}).
[[nodiscard]] core::Status decode_order_result(std::string_view json, PlaceAck& out,
                                               std::string& error);

// X-MBX-USED-WEIGHT-<n><unit> and X-MBX-ORDER-COUNT-<n><unit> response headers.
void feedback_from_header(std::string_view name, std::string_view value, core::UnixNanos recv,
                          std::vector<model::RateLimitFeedback>& out);

} // namespace jarvis::adapter::binance
