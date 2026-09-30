#include "jarvis/node/admin_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
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
  constexpr std::array<std::pair<std::string_view, model::AdminAction>, 6> kWords{{
      {"halt", model::AdminAction::Halt},
      {"reduce", model::AdminAction::Reduce},
      {"resume", model::AdminAction::Resume},
      {"cancel_all", model::AdminAction::CancelAll},
      {"shutdown", model::AdminAction::Shutdown},
      {"snapshot", model::AdminAction::Snapshot},
  }};
  for (const auto& [name, action] : kWords) {
    if (name == word) {
      return action;
    }
  }
  return std::nullopt;
}

namespace {

std::string_view next_word(std::string_view& rest) {
  const std::size_t start = rest.find_first_not_of(' ');
  if (start == std::string_view::npos) {
    rest = {};
    return {};
  }
  rest.remove_prefix(start);
  const std::size_t end = rest.find(' ');
  const std::string_view word = rest.substr(0, end);
  rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
  return word;
}

// true/false, a signed decimal integer, or text (quotes around it are dropped).
void typed_value(std::string_view text, model::ParamUpdate& out) {
  if (text == "true" || text == "false") {
    out.kind = model::ParamKind::Bool;
    out.integer = text == "true" ? 1 : 0;
    return;
  }
  std::int64_t v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (!text.empty() && ec == std::errc{} && end == text.data() + text.size()) {
    out.kind = model::ParamKind::Int;
    out.integer = v;
    return;
  }
  out.kind = model::ParamKind::Text;
  out.integer = 0;
}

} // namespace

std::string parse_admin_request(std::string_view line, std::span<const std::string> strategies,
                                AdminRequest& out) {
  out = AdminRequest{};
  std::string_view rest = line;
  const std::string_view word = next_word(rest);
  if (word != "set_param") {
    const std::optional<model::AdminAction> action = admin_action(word);
    if (!action || !next_word(rest).empty()) {
      return "unknown command \"" + std::string{line} +
             "\" (halt, reduce, resume, cancel_all, shutdown, snapshot, set_param, status)";
    }
    out.action = *action;
    return {};
  }
  const std::string_view id = next_word(rest);
  const std::string_view key = next_word(rest);
  const std::size_t start = rest.find_first_not_of(' ');
  std::string_view value =
      start == std::string_view::npos ? std::string_view{} : rest.substr(start);
  if (id.empty() || key.empty() || value.empty()) {
    return "set_param needs a strategy id, a key and a value";
  }
  const auto found = std::find(strategies.begin(), strategies.end(), id);
  if (found == strategies.end()) {
    return "no strategy \"" + std::string{id} + "\" in this node";
  }
  out.is_param = true;
  out.param.strategy_index = static_cast<std::uint16_t>(found - strategies.begin());
  if (!core::ok(model::ParamKey::from(key, out.param.key))) {
    return "the key is longer than " + std::to_string(model::ParamKey::capacity()) + " bytes";
  }
  const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
  if (quoted) {
    value = value.substr(1, value.size() - 2);
  }
  if (!core::ok(model::ParamText::from(value, out.param.text))) {
    return "the value is longer than " + std::to_string(model::ParamText::capacity()) + " bytes";
  }
  if (quoted) {
    out.param.kind = model::ParamKind::Text;
  } else {
    typed_value(value, out.param);
  }
  return {};
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
