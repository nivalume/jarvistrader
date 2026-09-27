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

namespace jarvis::network {

struct WsConfig {
  std::string url;
  TlsOptions tls;
  std::vector<HeaderField> headers;
  std::size_t receive_buffer = std::size_t{64} << 10U;
  std::size_t max_message = std::size_t{16} << 20U;
  std::chrono::milliseconds handshake_timeout{10'000};
};

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
