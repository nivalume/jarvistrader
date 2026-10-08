#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/network/io.hpp"
#include "jarvis/network/ws_frame.hpp"

// A WebSocket client over TLS (or plain TCP for ws:// URLs) on one IoContext (docs/architecture.md
// section 13.2). The receive buffer is preallocated and frames are decoded in place; text and
// binary messages go to on_message with the time they arrived; pings are answered with pongs;
// a close, an error or a failed handshake ends in on_close exactly once. Reconnecting is the
// caller's decision (network/backoff.hpp).
//
// Liveness (docs/architecture.md section 13.2): a connection that dies without a FIN or RST (a
// VM snapshot or fork, a NAT table that forgot the flow, a cable pulled) is invisible to a
// reader. With `idle_timeout` set, every received byte re-arms a read-idle deadline; after
// idle_timeout / 2 without one the client sends a ping, and when even the pong does not come
// the connection fails with a reason that starts with kWsIdleTimeout, so the caller's normal
// on_close path (reconnect with backoff, resync) runs. SO_KEEPALIVE and, on Linux,
// TCP_USER_TIMEOUT make the kernel give up on a dead path too.

namespace jarvis::network {

struct WsConfig {
  std::string url;
  TlsOptions tls;
  std::vector<HeaderField> headers;
  std::size_t receive_buffer = std::size_t{64} << 10U;
  std::size_t max_message = std::size_t{16} << 20U;
  std::chrono::milliseconds handshake_timeout{10'000};
  // Read-idle deadline of an open connection: no byte received for this long fails it. Zero: no
  // deadline. Any received data, ping or pong counts.
  std::chrono::milliseconds idle_timeout{0};
  // With an idle deadline, a ping goes out when the connection has been quiet for ping_interval
  // (zero: idle_timeout / 2), and its pong counts as activity; a quiet but healthy stream is
  // therefore never failed. false: no client pings (the server's own pings must keep it alive).
  bool client_ping = true;
  std::chrono::milliseconds ping_interval{0};
  // Kernel side. SO_KEEPALIVE with these probe timings (Linux and macOS; elsewhere the
  // system defaults), and TCP_USER_TIMEOUT (Linux only): data left unacknowledged this long
  // closes the socket. Zero tcp_user_timeout: idle_timeout (off when that is off).
  bool keepalive = true;
  std::chrono::seconds keepalive_idle{15};
  std::chrono::seconds keepalive_interval{5};
  int keepalive_count = 3;
  std::chrono::milliseconds tcp_user_timeout{0};
};

// The on_close reason of a connection failed by its idle deadline starts with this.
inline constexpr std::string_view kWsIdleTimeout = "idle timeout";

struct WsHandlers {
  std::function<void()> on_open;
  std::function<void(WsOpcode, std::span<const std::byte>, std::int64_t recv_ns)> on_message;
  std::function<void(const std::string& reason)> on_close;
};

struct WsStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t pings = 0;
  std::uint64_t sent = 0;
  std::uint64_t pongs = 0;         // received
  std::uint64_t client_pings = 0;  // sent because the connection was quiet
  std::uint64_t idle_timeouts = 0; // connections failed by the idle deadline
};

class WsClient {
public:
  WsClient(IoContext& io, WsConfig config, WsHandlers handlers);
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  // Starts connecting; the result arrives as on_open or on_close. Call from the IO thread (or
  // before it runs).
  void connect();
  // Queues a text message (thread-safe: posted to the IO thread).
  void send_text(std::string payload);
  // Starts the closing handshake (thread-safe).
  void close(std::uint16_t code = 1000, std::string reason = {});

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] WsStats stats() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace jarvis::network
