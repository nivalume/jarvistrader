#include "jarvis/node/admin_protocol.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace jarvis::node {

namespace {

using core::Status;

// A socket closed on every path out.
class Fd {
public:
  explicit Fd(int fd) noexcept : fd_{fd} {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

constexpr int kTimeoutMs = 5'000;

} // namespace

std::string admin_socket_path(const NodeConfig& config) {
  std::string path = config.admin.socket;
  constexpr std::string_view scheme = "unix://";
  if (path.starts_with(scheme)) {
    path.erase(0, scheme.size());
  }
  const std::string key = "{node_id}";
  for (std::size_t at = path.find(key); at != std::string::npos; at = path.find(key)) {
    path.replace(at, key.size(), config.node.id);
  }
  return path;
}

std::optional<model::AdminAction> admin_action(std::string_view word) noexcept {
  constexpr std::array<std::pair<std::string_view, model::AdminAction>, 5> kWords{{
      {"halt", model::AdminAction::Halt},
      {"reduce", model::AdminAction::Reduce},
      {"resume", model::AdminAction::Resume},
      {"cancel_all", model::AdminAction::CancelAll},
      {"shutdown", model::AdminAction::Shutdown},
  }};
  for (const auto& [name, action] : kWords) {
    if (name == word) {
      return action;
    }
  }
  return std::nullopt;
}

Status admin_request(const std::string& path, std::string_view line, std::string& reply,
                     std::string& error) {
  reply.clear();
  sockaddr_un addr{};
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    error = "admin socket path \"" + path + "\" is empty or too long";
    return Status::InvalidArgument;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.data(), path.size());
  const Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (fd.get() < 0 ||
      ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), // NOLINT: the sockets API
                sizeof(addr)) != 0) {
    error =
        "cannot connect to " + path + ": " + std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
    return Status::IoError;
  }
  const std::string out = std::string{line} + "\n";
  if (::send(fd.get(), out.data(), out.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(out.size())) {
    error = "cannot send to " + path;
    return Status::IoError;
  }
  std::array<char, 4096> buffer{};
  while (reply.find('\n') == std::string::npos) {
    pollfd p{fd.get(), POLLIN, 0};
    if (::poll(&p, 1, kTimeoutMs) <= 0) {
      error = "no reply from " + path;
      return Status::IoError;
    }
    const ssize_t n = ::recv(fd.get(), buffer.data(), buffer.size(), 0);
    if (n <= 0) {
      break;
    }
    reply.append(buffer.data(), static_cast<std::size_t>(n));
  }
  if (const std::size_t end = reply.find('\n'); end != std::string::npos) {
    reply.resize(end);
    return Status::Ok;
  }
  error = "the reply from " + path + " was cut short";
  return Status::IoError;
}

} // namespace jarvis::node
