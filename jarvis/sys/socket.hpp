#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"

// Blocking sockets for the node's small servers and clients: the admin socket (a Unix domain
// socket), the telemetry HTTP endpoint and test helpers. The venue connections use Asio
// (jarvis/network) instead.
//
//   Linux    BSD sockets, close-on-exec, MSG_NOSIGNAL
//   macOS    BSD sockets, close-on-exec set with fcntl, SO_NOSIGPIPE
//   Windows  Winsock 2 (started on first use), WSAPoll, AF_UNIX (Windows 10 1803 and later),
//            sockets not inherited by child processes
//
// No platform header is included here: a socket is an integer of the platform's width.

namespace jarvis::sys {

#if defined(_WIN32)
using Socket = std::uintptr_t; // SOCKET
inline constexpr Socket kNoSocket = ~Socket{0};
#else
using Socket = int;
inline constexpr Socket kNoSocket = -1;
#endif

// A socket closed when it goes out of scope.
class SocketHandle {
public:
  SocketHandle() = default;
  explicit SocketHandle(Socket s) noexcept : socket_{s} {}
  ~SocketHandle() { reset(); }
  SocketHandle(SocketHandle&& other) noexcept : socket_{other.release()} {}
  SocketHandle& operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  [[nodiscard]] Socket get() const noexcept { return socket_; }
  [[nodiscard]] bool valid() const noexcept { return socket_ != kNoSocket; }
  Socket release() noexcept {
    const Socket s = socket_;
    socket_ = kNoSocket;
    return s;
  }
  void reset(Socket s = kNoSocket) noexcept;

private:
  Socket socket_ = kNoSocket;
};

// Listens on a Unix domain socket at `path`, replacing a file a stopped node left there. Where
// files carry permission bits the socket file is created 0600: only its owner can connect.
[[nodiscard]] core::Status listen_unix(const std::string& path, int backlog, SocketHandle& out,
                                       std::string& error);
[[nodiscard]] core::Status connect_unix(const std::string& path, SocketHandle& out,
                                        std::string& error);
// The longest path listen_unix and connect_unix accept.
[[nodiscard]] std::size_t max_unix_path() noexcept;
// False on a Windows older than 10 version 1803 (and under Wine before AF_UNIX support), where
// listen_unix and connect_unix fail.
[[nodiscard]] bool unix_sockets_available() noexcept;

// Listens on TCP `host`:`port` ("" for every address; port "0" lets the system pick).
// `bound_port` is the port listened on.
[[nodiscard]] core::Status listen_tcp(const std::string& host, const std::string& port, int backlog,
                                      SocketHandle& out, std::uint16_t& bound_port,
                                      std::string& error);
[[nodiscard]] core::Status connect_tcp(const std::string& host, std::uint16_t port,
                                       SocketHandle& out, std::string& error);

// The next connection on `listener` (invalid on failure).
[[nodiscard]] SocketHandle accept_connection(Socket listener) noexcept;
// Waits until `s` has data or was closed by the peer: > 0 ready, 0 timed out, < 0 failed.
[[nodiscard]] int wait_readable(Socket s, int timeout_ms) noexcept;
// Bytes received, 0 when the peer closed, -1 on failure.
[[nodiscard]] long receive(Socket s, void* data, std::size_t size) noexcept;
// Bytes sent or -1; a closed peer never raises SIGPIPE.
[[nodiscard]] long send_some(Socket s, const void* data, std::size_t size) noexcept;
// Sends all of `data`; false on failure.
[[nodiscard]] bool send_all(Socket s, std::string_view data) noexcept;
// Receive and send timeouts (SO_RCVTIMEO and SO_SNDTIMEO).
void set_timeouts(Socket s, int milliseconds) noexcept;
// The calling thread's last socket error, as text.
[[nodiscard]] std::string socket_error_text();

} // namespace jarvis::sys
