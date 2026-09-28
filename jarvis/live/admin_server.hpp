#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "jarvis/core/status.hpp"
#include "jarvis/live/spsc_ring.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/generated/enums.hpp"

// The admin socket's server (docs/architecture.md section 19.3; the protocol is in
// node/admin_protocol.hpp). Its own thread accepts one command per connection on a Unix domain
// socket readable by the owner only (mode 0600). A command is handed to the core thread through
// a SPSC ring, which the pump drains before everything else, and becomes a recorded
// AdminCommand; "status" is answered from what the core thread last published:
//
//   ready  the node is Running (synced, and no connection known to be down);
//   alive  the core loop published within the last five seconds.

namespace jarvis::live {

// What the core thread publishes every round of its loop.
struct NodeStatus {
  std::atomic<std::uint8_t> state{0};   // model::NodeState
  std::atomic<std::uint8_t> trading{0}; // model::TradingState
  std::atomic<std::uint64_t> seq{0};
  std::atomic<std::int64_t> round_ns{0}; // steady clock of the last round; 0: none yet

  void publish(model::NodeState s, model::TradingState t, std::uint64_t last_seq,
               std::int64_t steady_ns) noexcept {
    state.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed);
    trading.store(static_cast<std::uint8_t>(t), std::memory_order_relaxed);
    seq.store(last_seq, std::memory_order_relaxed);
    round_ns.store(steady_ns, std::memory_order_release);
  }
};

class AdminServer {
public:
  explicit AdminServer(std::string path, std::size_t slots = 64);
  ~AdminServer();
  AdminServer(const AdminServer&) = delete;
  AdminServer& operator=(const AdminServer&) = delete;

  // Binds the socket (a stale socket file is replaced) and starts the thread.
  [[nodiscard]] core::Status start(std::string& error);
  void stop(); // joins the thread and removes the socket file

  [[nodiscard]] SpscRing<model::AdminAction>& commands() noexcept; // admin -> core
  [[nodiscard]] NodeStatus& status() noexcept;
  [[nodiscard]] std::uint64_t accepted() const noexcept; // commands handed to the core
  [[nodiscard]] const std::string& path() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace jarvis::live
