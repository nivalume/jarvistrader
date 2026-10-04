#include "jarvis/live/admin_server.hpp"

#include <array>
#include <filesystem>
#include <string_view>
#include <thread>

#include "jarvis/live/cpu_affinity.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/node/admin_protocol.hpp"
#include "jarvis/sys/socket.hpp"

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
  sys::SocketHandle listener;
  std::atomic<bool> stopping{false};

  // One command: read a line (bounded in size and time), answer it.
  void serve(sys::Socket fd) {
    std::string line;
    std::array<char, 512> buffer{};
    while (line.find('\n') == std::string::npos && line.size() < buffer.size()) {
      if (sys::wait_readable(fd, kReadMs) <= 0) {
        return;
      }
      const long n = sys::receive(fd, buffer.data(), buffer.size());
      if (n <= 0) {
        return;
      }
      line.append(buffer.data(), static_cast<std::size_t>(n));
    }
    line.resize(line.find_first_of("\r\n") == std::string::npos ? line.size()
                                                                : line.find_first_of("\r\n"));
    const std::string reply = answer(line) + "\n";
    static_cast<void>(sys::send_all(fd, reply));
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
      if (sys::wait_readable(listener.get(), kPollMs) <= 0) {
        continue;
      }
      const sys::SocketHandle client = sys::accept_connection(listener.get());
      if (client.valid()) {
        serve(client.get());
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
  // The socket file is created 0600 where files carry permission bits; a stale one left by a
  // node that did not stop cleanly is replaced.
  if (const Status s = sys::listen_unix(a.path, 8, a.listener, error); !core::ok(s)) {
    return s;
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
  a.listener.reset();
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
