#include "jarvis/live/admin_server.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "jarvis/live/cpu_affinity.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/node/admin_protocol.hpp"

namespace jarvis::live {

namespace {

using core::Status;

constexpr int kPollMs = 100;
constexpr int kReadMs = 1'000;
constexpr std::int64_t kAliveNs = 5'000'000'000;

std::string status_json(const NodeStatus& s) {
  const std::int64_t round = s.round_ns.load(std::memory_order_acquire);
  const auto state = static_cast<model::NodeState>(s.state.load(std::memory_order_relaxed));
  const auto trading = static_cast<model::TradingState>(s.trading.load(std::memory_order_relaxed));
  const bool alive = round != 0 && network::steady_ns() - round < kAliveNs;
  return R"({"state":")" + std::string{model::to_string(state)} + R"(","trading":")" +
         std::string{model::to_string(trading)} + R"(","seq":)" +
         std::to_string(s.seq.load(std::memory_order_relaxed)) + R"(,"ready":)" +
         (state == model::NodeState::Running ? "true" : "false") + R"(,"alive":)" +
         (alive ? "true" : "false") + "}";
}

} // namespace

struct AdminServer::Impl {
  Impl(std::string p, std::size_t slots) : ring{slots}, path{std::move(p)} {}

  SpscRing<node::AdminRequest> ring;
  std::atomic<std::uint64_t> accepted{0};
  std::thread thread;
  std::vector<std::string> strategies;
  std::vector<int> cpus;
  NodeStatus status;
  std::string path;
  int listener = -1;
  std::atomic<bool> stopping{false};

  // One command: read a line (bounded in size and time), answer it.
  void serve(int fd) {
    std::string line;
    std::array<char, 512> buffer{};
    while (line.find('\n') == std::string::npos && line.size() < buffer.size()) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, kReadMs) <= 0) {
        return;
      }
      const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (n <= 0) {
        return;
      }
      line.append(buffer.data(), static_cast<std::size_t>(n));
    }
    line.resize(line.find_first_of("\r\n") == std::string::npos ? line.size()
                                                                : line.find_first_of("\r\n"));
    const std::string reply = answer(line) + "\n";
    static_cast<void>(::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL));
  }

  std::string answer(std::string_view line) {
    if (line == "status") {
      return status_json(status);
    }
    node::AdminRequest request;
    if (const std::string refused = node::parse_admin_request(line, strategies, request);
        !refused.empty()) {
      return "error " + refused;
    }
    if (!ring.try_push(request)) {
      return "error busy: the core thread has not taken the earlier commands";
    }
    accepted.fetch_add(1, std::memory_order_relaxed);
    return "ok";
  }

  void run() {
    while (!stopping.load(std::memory_order_relaxed)) {
      pollfd p{listener, POLLIN, 0};
      if (::poll(&p, 1, kPollMs) <= 0) {
        continue;
      }
      const int fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) {
        serve(fd);
        ::close(fd);
      }
    }
  }
};

AdminServer::AdminServer(std::string path, std::size_t slots)
    : impl_{std::make_unique<Impl>(std::move(path), slots)} {}

AdminServer::~AdminServer() { stop(); }

Status AdminServer::start(std::string& error) {
  Impl& a = *impl_;
  if (a.thread.joinable()) {
    return Status::Ok;
  }
  sockaddr_un addr{};
  if (a.path.empty() || a.path.size() >= sizeof(addr.sun_path)) {
    error = "admin socket path \"" + a.path + "\" is empty or too long";
    return Status::InvalidArgument;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, a.path.data(), a.path.size());
  std::error_code ec;
  std::filesystem::remove(a.path, ec); // a socket left by a node that did not stop cleanly
  a.listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const mode_t before = ::umask(0177); // the socket file is created 0600
  const bool bound =
      a.listener >= 0 &&
      ::bind(a.listener, reinterpret_cast<const sockaddr*>(&addr), // NOLINT: the sockets API
             sizeof(addr)) == 0;
  ::umask(before);
  if (!bound || ::listen(a.listener, 8) != 0) {
    error =
        "cannot listen on " + a.path + ": " + std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
    if (a.listener >= 0) {
      ::close(a.listener);
      a.listener = -1;
    }
    return Status::IoError;
  }
  a.stopping.store(false);
  a.thread = std::thread{[&a] { a.run(); }};
  const Status pinned = pin_thread(a.thread, a.cpus, error);
  if (!core::ok(pinned)) {
    stop();
  }
  return pinned;
}

void AdminServer::stop() {
  Impl& a = *impl_;
  if (!a.thread.joinable()) {
    return;
  }
  a.stopping.store(true);
  a.thread.join();
  ::close(a.listener);
  a.listener = -1;
  std::error_code ec;
  std::filesystem::remove(a.path, ec);
}

void AdminServer::set_strategies(std::vector<std::string> ids) {
  impl_->strategies = std::move(ids);
}

void AdminServer::set_cpus(std::vector<int> cpus) { impl_->cpus = std::move(cpus); }

SpscRing<node::AdminRequest>& AdminServer::commands() noexcept { return impl_->ring; }
NodeStatus& AdminServer::status() noexcept { return impl_->status; }
std::uint64_t AdminServer::accepted() const noexcept {
  return impl_->accepted.load(std::memory_order_relaxed);
}
const std::string& AdminServer::path() const noexcept { return impl_->path; }

} // namespace jarvis::live
