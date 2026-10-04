#include "jarvis/sys/socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "jarvis/sys/error.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
// afunix.h needs winsock2.h first.
#include <afunix.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace jarvis::sys {

using core::Status;

namespace {

#if defined(_WIN32)

using Length = int;

bool started() noexcept {
  static const bool ok = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
}

Socket open_socket(int family, int type, int protocol) noexcept {
  if (!started()) {
    return kNoSocket;
  }
  const SOCKET s = ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  return s == INVALID_SOCKET ? kNoSocket : static_cast<Socket>(s);
}

void close_socket(Socket s) noexcept { ::closesocket(static_cast<SOCKET>(s)); }

#else

using Length = socklen_t;

bool started() noexcept { return true; }

// Close-on-exec, and no SIGPIPE from a closed peer on platforms without MSG_NOSIGNAL.
bool prepare(int fd) noexcept {
#if !defined(SOCK_CLOEXEC)
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) { // NOLINT(cppcoreguidelines-pro-type-vararg)
    return false;
  }
#endif
#if defined(SO_NOSIGPIPE)
  int yes = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) {
    return false;
  }
#endif
  static_cast<void>(fd);
  return true;
}

Socket open_socket(int family, int type, int protocol) noexcept {
#if defined(SOCK_CLOEXEC)
  const int fd = ::socket(family, type | SOCK_CLOEXEC, protocol);
#else
  const int fd = ::socket(family, type, protocol);
#endif
  if (fd >= 0 && !prepare(fd)) {
    ::close(fd);
    return kNoSocket;
  }
  return fd;
}

void close_socket(Socket s) noexcept { ::close(s); }

#endif

int last_error_code() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

// A Unix domain socket address for `path`, or false when the path does not fit.
bool unix_address(const std::string& path, sockaddr_un& addr) noexcept {
  addr = sockaddr_un{};
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.data(), path.size());
  return true;
}

const sockaddr* as_address(const sockaddr_un& addr) noexcept {
  return reinterpret_cast<const sockaddr*>(&addr); // NOLINT: the sockets API
}

// Why no Unix domain socket could be made for `path`.
std::string unix_socket_failure(const std::string& path) {
#if defined(_WIN32)
  if (::WSAGetLastError() == WSAEAFNOSUPPORT) {
    return "cannot create a socket for " + path +
           ": this system has no Unix domain sockets (Windows 10 version 1803 or later has them)";
  }
#endif
  return "cannot create a socket for " + path + ": " + socket_error_text();
}

} // namespace

void SocketHandle::reset(Socket s) noexcept {
  if (socket_ != kNoSocket) {
    close_socket(socket_);
  }
  socket_ = s;
}

std::size_t max_unix_path() noexcept { return sizeof(sockaddr_un::sun_path) - 1; }

bool unix_sockets_available() noexcept {
#if defined(_WIN32)
  static const bool available = [] {
    const Socket s = open_socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == kNoSocket) {
      return false;
    }
    close_socket(s);
    return true;
  }();
  return available;
#else
  return true;
#endif
}

Status listen_unix(const std::string& path, int backlog, SocketHandle& out, std::string& error) {
  sockaddr_un addr{};
  if (!unix_address(path, addr)) {
    error = "socket path \"" + path + "\" is empty or longer than " +
            std::to_string(max_unix_path()) + " bytes";
    return Status::InvalidArgument;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec); // a socket file left by a node that did not stop cleanly
  SocketHandle s{open_socket(AF_UNIX, SOCK_STREAM, 0)};
  if (!s.valid()) {
    error = unix_socket_failure(path);
    return Status::IoError;
  }
#if defined(_WIN32)
  const bool bound = ::bind(static_cast<SOCKET>(s.get()), as_address(addr), sizeof(addr)) == 0;
#else
  const mode_t before = ::umask(0177); // the socket file is created 0600
  const bool bound = ::bind(s.get(), as_address(addr), sizeof(addr)) == 0;
  ::umask(before);
#endif
  if (!bound || ::listen(s.get(), backlog) != 0) {
    error = "cannot listen on " + path + ": " + socket_error_text();
    return Status::IoError;
  }
  out = std::move(s);
  return Status::Ok;
}

Status connect_unix(const std::string& path, SocketHandle& out, std::string& error) {
  sockaddr_un addr{};
  if (!unix_address(path, addr)) {
    error = "socket path \"" + path + "\" is empty or longer than " +
            std::to_string(max_unix_path()) + " bytes";
    return Status::InvalidArgument;
  }
  SocketHandle s{open_socket(AF_UNIX, SOCK_STREAM, 0)};
  if (!s.valid()) {
    error = unix_socket_failure(path);
    return Status::IoError;
  }
  if (::connect(s.get(), as_address(addr), sizeof(addr)) != 0) {
    error = "cannot connect to " + path + ": " + socket_error_text();
    return Status::IoError;
  }
  out = std::move(s);
  return Status::Ok;
}

Status listen_tcp(const std::string& host, const std::string& port, int backlog, SocketHandle& out,
                  std::uint16_t& bound_port, std::string& error) {
  if (!started()) {
    error = "cannot start the socket library: " + socket_error_text();
    return Status::IoError;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* found = nullptr;
  if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), port.c_str(), &hints, &found) != 0 ||
      found == nullptr) {
    error = "cannot resolve " + host + ":" + port;
    return Status::InvalidArgument;
  }
  SocketHandle s{open_socket(found->ai_family, found->ai_socktype, found->ai_protocol)};
  bool bound = s.valid();
#if !defined(_WIN32)
  // Windows' SO_REUSEADDR would let another process take the port; its default is what this
  // asks for elsewhere: rebinding while old connections linger.
  int yes = 1;
  bound = bound && ::setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0;
#endif
  bound = bound && ::bind(s.get(), found->ai_addr, static_cast<Length>(found->ai_addrlen)) == 0 &&
          ::listen(s.get(), backlog) == 0;
  const std::string why = bound ? std::string{} : socket_error_text();
  ::freeaddrinfo(found);
  if (!bound) {
    error = "cannot listen on " + host + ":" + port + ": " + why;
    return Status::IoError;
  }
  sockaddr_storage addr{};
  Length length = sizeof(addr);
  bound_port = 0;
  if (::getsockname(s.get(), reinterpret_cast<sockaddr*>(&addr), &length) == 0) { // NOLINT
    if (addr.ss_family == AF_INET) {
      bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port); // NOLINT
    } else if (addr.ss_family == AF_INET6) {
      bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port); // NOLINT
    }
  }
  out = std::move(s);
  return Status::Ok;
}

Status connect_tcp(const std::string& host, std::uint16_t port, SocketHandle& out,
                   std::string& error) {
  if (!started()) {
    error = "cannot start the socket library: " + socket_error_text();
    return Status::IoError;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || found == nullptr) {
    error = "cannot resolve " + host;
    return Status::InvalidArgument;
  }
  SocketHandle s{open_socket(found->ai_family, found->ai_socktype, found->ai_protocol)};
  const bool connected =
      s.valid() && ::connect(s.get(), found->ai_addr, static_cast<Length>(found->ai_addrlen)) == 0;
  const std::string why = connected ? std::string{} : socket_error_text();
  ::freeaddrinfo(found);
  if (!connected) {
    error = "cannot connect to " + host + ":" + service + ": " + why;
    return Status::IoError;
  }
  out = std::move(s);
  return Status::Ok;
}

SocketHandle accept_connection(Socket listener) noexcept {
#if defined(_WIN32)
  const SOCKET s = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
  if (s == INVALID_SOCKET) {
    return SocketHandle{};
  }
  ::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); // NOLINT: Win32
  return SocketHandle{static_cast<Socket>(s)};
#elif defined(SOCK_CLOEXEC) && defined(__linux__)
  return SocketHandle{::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC)};
#else
  const int fd = ::accept(listener, nullptr, nullptr);
  if (fd < 0) {
    return SocketHandle{};
  }
#if defined(SOCK_CLOEXEC)
  static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC)); // NOLINT(cppcoreguidelines-pro-type-vararg)
#endif
  if (!prepare(fd)) {
    ::close(fd);
    return SocketHandle{};
  }
  return SocketHandle{fd};
#endif
}

int wait_readable(Socket s, int timeout_ms) noexcept {
#if defined(_WIN32)
  WSAPOLLFD p{};
  p.fd = static_cast<SOCKET>(s);
  p.events = POLLRDNORM;
  return ::WSAPoll(&p, 1, timeout_ms);
#else
  pollfd p{s, POLLIN, 0};
  return ::poll(&p, 1, timeout_ms);
#endif
}

long receive(Socket s, void* data, std::size_t size) noexcept {
#if defined(_WIN32)
  const int n = ::recv(static_cast<SOCKET>(s), static_cast<char*>(data),
                       static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
  return n == SOCKET_ERROR ? -1 : static_cast<long>(n);
#else
  return static_cast<long>(::recv(s, data, size, 0));
#endif
}

long send_some(Socket s, const void* data, std::size_t size) noexcept {
#if defined(_WIN32)
  const int n = ::send(static_cast<SOCKET>(s), static_cast<const char*>(data),
                       static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
  return n == SOCKET_ERROR ? -1 : static_cast<long>(n);
#elif defined(MSG_NOSIGNAL)
  return static_cast<long>(::send(s, data, size, MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(s, data, size, 0)); // SO_NOSIGPIPE is set on the socket
#endif
}

bool send_all(Socket s, std::string_view data) noexcept {
  while (!data.empty()) {
    const long n = send_some(s, data.data(), data.size());
    if (n <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

void set_timeouts(Socket s, int milliseconds) noexcept {
#if defined(_WIN32)
  const auto ms = static_cast<DWORD>(milliseconds);
  const auto* value = reinterpret_cast<const char*>(&ms); // NOLINT: the sockets API
  ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, value, sizeof(ms));
  ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_SNDTIMEO, value, sizeof(ms));
#else
  timeval timeout{};
  timeout.tv_sec = milliseconds / 1000;
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((milliseconds % 1000) * 1000);
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

std::string socket_error_text() {
  return system_error_text(static_cast<unsigned long>(last_error_code()));
}

} // namespace jarvis::sys
