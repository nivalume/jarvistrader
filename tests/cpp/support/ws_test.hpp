#pragma once

// A loopback WebSocket venue for tests: RFC 6455 server frames, reading masked client frames,
// and a scripted WSS server that answers each client message with the replies a test function
// returns (docs/architecture.md section 17.2, the mock venue).

#include <poll.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "jarvis/network/ws_frame.hpp"
#include "tls_test.hpp"

namespace jarvis::testsupport {

// A server frame (unmasked); `first` is the FIN bit and opcode.
inline std::vector<std::byte> ws_server_frame(unsigned first, std::string_view payload) {
  std::vector<std::byte> out{static_cast<std::byte>(first)};
  const std::size_t n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<std::byte>(n));
  } else if (n <= 0xFFFF) {
    out.push_back(static_cast<std::byte>(126));
    out.push_back(static_cast<std::byte>(n >> 8U));
    out.push_back(static_cast<std::byte>(n & 0xFFU));
  } else {
    out.push_back(static_cast<std::byte>(127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(n) >> shift) & 0xFFU));
    }
  }
  for (const char c : payload) {
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

// Reads one masked client frame from a blocking stream: opcode and unmasked payload.
template <typename Stream> std::pair<unsigned, std::string> read_ws_client_frame(Stream& s) {
  std::array<unsigned char, 2> head{};
  asio::read(s, asio::buffer(head));
  std::uint64_t len = head[1] & 0x7FU;
  if (len == 126) {
    std::array<unsigned char, 2> ext{};
    asio::read(s, asio::buffer(ext));
    len = (static_cast<std::uint64_t>(ext[0]) << 8U) | ext[1];
  } else if (len == 127) {
    std::array<unsigned char, 8> ext{};
    asio::read(s, asio::buffer(ext));
    len = 0;
    for (const unsigned char b : ext) {
      len = (len << 8U) | b;
    }
  }
  if ((head[1] & 0x80U) == 0) {
    throw std::runtime_error{"an unmasked client frame"};
  }
  std::array<unsigned char, 4> mask{};
  asio::read(s, asio::buffer(mask));
  std::string payload(len, '\0');
  asio::read(s, asio::buffer(payload));
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
  }
  return {head[0] & 0x0FU, payload};
}

struct WsReply {
  enum class Kind : std::uint8_t { Text, Close, Drop, Silent };
  Kind kind = Kind::Text;
  std::string text;

  static WsReply send(std::string t) { return {Kind::Text, std::move(t)}; }
  static WsReply close() { return {Kind::Close, {}}; }
  static WsReply drop() { return {Kind::Drop, {}}; } // no close frame
  // The connection stays open and the server stops reading and writing, for good: no pong, no
  // data, no close, no FIN or RST, as a peer behind a dead path looks to the client (the
  // connection is only closed when the server is destroyed).
  static WsReply silent() { return {Kind::Silent, {}}; }
};

// Called with the connection number (from 0) and each text message; the empty message stands
// for the connection's opening, before any client message.
using WsScript = std::function<std::vector<WsReply>(std::size_t conn, const std::string& message)>;

// Serves up to `connections` WebSocket connections over TLS, each on its own thread (a planned
// rotation keeps two open at once). Records each connection's request target and every text
// message received; the script is called under a lock, one message at a time. A test can also
// push replies to a connection unprompted, WsReply::silent() among them. Client pings are
// answered with pongs, so a quiet connection stays alive until it goes silent.
class ScriptedWssServer {
public:
  ScriptedWssServer(std::size_t connections, WsScript script)
      : ssl_{asio::ssl::context::tls_server}, connections_{connections},
        script_{std::move(script)} {
    make_certificate(dir_.file("cert.pem"), dir_.file("key.pem"));
    ssl_.use_certificate_chain_file(dir_.file("cert.pem"));
    ssl_.use_private_key_file(dir_.file("key.pem"), asio::ssl::context::pem);
    acceptor_.open(asio::ip::tcp::v4());
    acceptor_.bind({asio::ip::make_address("127.0.0.1"), 0});
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread{[this] { accept_loop(); }};
  }
  ~ScriptedWssServer() {
    stopping_ = true;
    if (thread_.joinable()) {
      wake();
      thread_.join();
    }
    for (std::thread& t : sessions_) {
      t.join();
    }
  }
  ScriptedWssServer(const ScriptedWssServer&) = delete;
  ScriptedWssServer& operator=(const ScriptedWssServer&) = delete;

  [[nodiscard]] std::string ca_file() const { return dir_.file("cert.pem"); }
  [[nodiscard]] std::string key_file() const { return dir_.file("key.pem"); }
  [[nodiscard]] std::string url(std::string_view target) const {
    return "wss://localhost:" + std::to_string(port_) + std::string{target};
  }
  [[nodiscard]] std::vector<std::string> targets() {
    const std::lock_guard lock{mutex_};
    return targets_;
  }
  [[nodiscard]] std::vector<std::pair<std::size_t, std::string>> messages() {
    const std::lock_guard lock{mutex_};
    return messages_;
  }
  [[nodiscard]] std::string error() {
    const std::lock_guard lock{mutex_};
    return error_;
  }
  // The request target of connection `conn` (empty before its upgrade); a script may call it.
  [[nodiscard]] std::string target(std::size_t conn) const {
    const std::lock_guard lock{mutex_};
    const auto it = by_conn_.find(conn);
    return it == by_conn_.end() ? std::string{} : it->second;
  }
  [[nodiscard]] std::size_t served() const { return served_.load(); } // connections ended
  [[nodiscard]] std::size_t accepted() const {                        // connections upgraded
    const std::lock_guard lock{mutex_};
    return targets_.size();
  }
  // Sends `reply` on connection `conn` (from 0) at its next turn, unprompted.
  void push(std::size_t conn, WsReply reply) {
    const std::lock_guard lock{mutex_};
    pushed_[conn].push_back(std::move(reply));
  }

private:
  using Stream = asio::ssl::stream<asio::ip::tcp::socket>;

  // Unblocks the accept of a connection that never came.
  void wake() const noexcept {
    try {
      asio::error_code ec;
      asio::io_context io;
      asio::ip::tcp::socket s{io};
      s.connect({asio::ip::make_address("127.0.0.1"), port_}, ec);
    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): nothing to unblock then
    }
  }

  void accept_loop() {
    for (std::size_t conn = 0; conn < connections_; ++conn) {
      auto socket = std::make_unique<asio::ip::tcp::socket>(io_);
      asio::error_code ec;
      acceptor_.accept(*socket, ec);
      if (ec || stopping_) {
        return;
      }
      sessions_.emplace_back(
          [this, conn, s = std::move(socket)]() mutable { serve(std::move(*s), conn); });
    }
  }

  void serve(asio::ip::tcp::socket socket, std::size_t conn) {
    Stream s{std::move(socket), ssl_};
    try {
      s.handshake(asio::ssl::stream_base::server);
      const std::string head = read_head(s);
      {
        const std::lock_guard lock{mutex_};
        targets_.push_back(head.substr(4, head.find(' ', 4) - 4));
        by_conn_[conn] = targets_.back();
      }
      const std::string response =
          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Accept: " +
          network::ws_accept_for(header_value(head, "Sec-WebSocket-Key")) + "\r\n\r\n";
      asio::write(s, asio::buffer(response));
      session(s, conn);
    } catch (const std::exception& e) {
      // The client going away mid-read ends a connection; anything else is a test failure.
      const std::string what = e.what();
      if (what.find("end of file") == std::string::npos &&
          what.find("reset") == std::string::npos &&
          what.find("stream truncated") == std::string::npos) {
        const std::lock_guard lock{mutex_};
        error_ = what;
      }
    }
    asio::error_code ec;
    s.lowest_layer().close(ec);
    ++served_;
  }

  std::vector<WsReply> run_script(std::size_t conn, const std::string& message) {
    const std::lock_guard lock{script_mutex_};
    return script_(conn, message);
  }

  // False when the connection ends.
  bool apply(Stream& s, const std::vector<WsReply>& replies) const {
    for (const WsReply& r : replies) {
      switch (r.kind) {
      case WsReply::Kind::Text:
        asio::write(s, asio::buffer(ws_server_frame(0x81, r.text)));
        break;
      case WsReply::Kind::Close: {
        const std::string payload{"\x03\xe8", 2}; // 1000
        asio::write(s, asio::buffer(ws_server_frame(0x88, payload)));
        for (;;) { // until the client's close
          if (read_ws_client_frame(s).first == 0x8) {
            return false;
          }
        }
      }
      case WsReply::Kind::Drop:
        return false;
      case WsReply::Kind::Silent:
        while (!stopping_) {
          std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
      }
    }
    return true;
  }

  // Replies pushed by the test for this connection, taken out under the lock.
  std::vector<WsReply> take_pushed(std::size_t conn) {
    const std::lock_guard lock{mutex_};
    std::vector<WsReply> out;
    if (const auto it = pushed_.find(conn); it != pushed_.end()) {
      out.swap(it->second);
    }
    return out;
  }

  // Waits up to a millisecond for input from the client: records already read from the socket
  // together with earlier ones (in OpenSSL or in asio's BIO pair), or bytes on the socket.
  static bool readable(Stream& s) {
    SSL* ssl = s.native_handle();
    if (SSL_pending(ssl) > 0 || SSL_has_pending(ssl) == 1 ||
        BIO_ctrl_pending(SSL_get_rbio(ssl)) > 0) {
      return true;
    }
    pollfd p{s.lowest_layer().native_handle(), POLLIN, 0};
    return ::poll(&p, 1, 1) > 0;
  }

  void session(Stream& s, std::size_t conn) {
    if (!apply(s, run_script(conn, {}))) {
      return;
    }
    for (;;) {
      if (!apply(s, take_pushed(conn))) {
        return;
      }
      if (!readable(s)) {
        continue;
      }
      auto [opcode, payload] = read_ws_client_frame(s);
      if (opcode == 0x8) {
        asio::write(s, asio::buffer(ws_server_frame(0x88, payload)));
        return;
      }
      if (opcode == 0x9) {
        asio::write(s, asio::buffer(ws_server_frame(0x8A, payload)));
        continue;
      }
      if (opcode != 0x1) {
        continue;
      }
      {
        const std::lock_guard lock{mutex_};
        messages_.emplace_back(conn, payload);
      }
      if (!apply(s, run_script(conn, payload))) {
        return;
      }
    }
  }

  TempDir dir_;
  asio::io_context io_;
  asio::ssl::context ssl_;
  asio::ip::tcp::acceptor acceptor_{io_};
  std::uint16_t port_ = 0;
  std::size_t connections_;
  WsScript script_;
  std::mutex script_mutex_;
  mutable std::mutex mutex_;
  std::map<std::size_t, std::vector<WsReply>> pushed_;
  std::vector<std::string> targets_;
  std::map<std::size_t, std::string> by_conn_;
  std::vector<std::pair<std::size_t, std::string>> messages_;
  std::string error_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> served_{0};
  std::vector<std::thread> sessions_; // written by the accept thread only
  std::thread thread_;
};

} // namespace jarvis::testsupport
