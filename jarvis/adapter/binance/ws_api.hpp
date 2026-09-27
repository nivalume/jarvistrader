#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "jarvis/adapter/binance/order_tracker.hpp"
#include "jarvis/adapter/binance/requests.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/signer.hpp"
#include "jarvis/network/timer.hpp"

// The order channel over the WebSocket API (docs/architecture.md section 14.4), on the
// order-sender's IoContext; every member is called on that thread.
//
// - An Ed25519 key logs the connection on with session.logon, after which requests carry no
//   signature; any other key signs every request.
// - Requests are matched to answers by id. An order request ends in one outcome: acknowledged,
//   refused (the venue's code), or unknown (no answer before the timeout, or the connection went
//   down with the request in flight). An unknown order stays for reconciliation to settle; an
//   answer that arrives after its timeout is still reported.
// - The venue closes connections after 24 hours. Before that the session opens a second
//   connection, logs it on, sends new requests there and closes the old one once its requests
//   are answered, so order entry never pauses for a planned rotation.
// - A dropped connection reconnects with backoff; a refused logon does not (the key is wrong).

namespace jarvis::adapter::binance {

struct WsApiConfig {
  std::string url = "wss://ws-fapi.binance.com/ws-fapi/v1";
  std::string api_key;
  network::Signer signer;
  network::TlsOptions tls;
  std::int64_t recv_window_ms = 5000;
  std::chrono::milliseconds request_timeout{10'000};
  std::chrono::milliseconds reconnect_initial{500};
  std::chrono::milliseconds reconnect_max{30'000};
  std::chrono::milliseconds rotate_after{std::chrono::hours{23}};
  std::function<std::int64_t()> now_ms; // local UTC milliseconds; system_clock when empty
};

enum class OutcomeKind : std::uint8_t { Acknowledged, Refused, Unknown };

struct OrderOutcome {
  OutcomeKind kind = OutcomeKind::Unknown;
  RequestKind request = RequestKind::Place;
  std::string client_order_id;
  PlaceAck ack;       // Acknowledged
  RequestError error; // Refused
  std::string reason; // Unknown: "timeout" or why the connection closed
  std::int64_t recv_ns = 0;
};

struct WsApiHandlers {
  std::function<void()> on_ready;                  // requests can be sent
  std::function<void(const std::string&)> on_down; // requests cannot be sent (fall back)
  std::function<void(const OrderOutcome&)> on_outcome;
  std::function<void(std::span<const model::RateLimitFeedback>)> on_limits;
};

struct WsApiStats {
  std::uint64_t sent = 0;
  std::uint64_t acknowledged = 0;
  std::uint64_t refused = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t lost = 0;         // in flight when a connection went down
  std::uint64_t late_answers = 0; // answered after the timeout
  std::uint64_t unmatched = 0;    // answers to no known request
  std::uint64_t connects = 0;
  std::uint64_t rotations = 0;
  std::uint64_t decode_errors = 0;
};

class WsApiSession {
public:
  // The answer to a request made with request(): Ok with the answer, or IoError when there was
  // none (timeout, connection down).
  using Callback = std::function<void(core::Status, const WsAnswer&, std::string_view json)>;

  WsApiSession(network::IoContext& io, WsApiConfig config, WsApiHandlers handlers);
  ~WsApiSession();
  WsApiSession(const WsApiSession&) = delete;
  WsApiSession& operator=(const WsApiSession&) = delete;

  void start();
  void stop(); // closes every connection; no reconnect

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] WsApiStats stats() const noexcept;
  void set_time_offset(std::int64_t ms) noexcept; // the venue's clock minus the local one

  // Order requests. InvalidState when no connection is ready (the caller falls back to REST),
  // InvalidArgument when the command cannot be expressed; otherwise the outcome follows.
  [[nodiscard]] core::Status place(const model::SubmitOrder& c, std::string_view symbol,
                                   std::string& error);
  [[nodiscard]] core::Status modify(const model::ModifyOrder& c, std::string_view symbol,
                                    model::OrderSide side, std::string& error);
  [[nodiscard]] core::Status cancel(const model::CancelOrder& c, std::string_view symbol,
                                    std::string& error);
  // Any other method (order.status, userDataStream.start, ...).
  [[nodiscard]] core::Status request(std::string_view method, Params params, Security security,
                                     Callback callback, std::string& error);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace jarvis::adapter::binance
