#pragma once

#include <optional>

#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/execution/reconciliation.hpp"

// The driver's sync gate (docs/architecture.md sections 4.4 and 15.1): the lifecycle move the
// account's reconciliation phase and the node's other connections call for. The driver applies
// it after every input until it returns nothing, recording each transition, so a replay needs no
// gate of its own.
//
//   Syncing   -> Running    once the account is synced and no connection is down (with
//                           `await_stream`, a node that has never heard from the user data stream
//                           is not synced, and order entry must have come up: live waits for
//                           both);
//   Syncing   -> Degraded   the stream dropped while reconciling;
//   Running   -> Degraded   the stream dropped (the kernel halted trading in the same step), or
//                           market data or order entry went down;
//   Degraded  -> Syncing    the stream is back and the other connections are up: reconciliation
//                           runs again.
//
// Without `await_stream` (backtest, sandbox) a node without a user stream counts as synced, so
// only its market data connection can move it.

namespace jarvis::engine {

[[nodiscard]] constexpr std::optional<LifecycleReason>
sync_move(NodeState state, execution::SyncPhase phase, bool await_stream,
          execution::ConnectionHealth health) noexcept {
  using execution::SyncPhase;
  const bool local = phase == SyncPhase::Local && !await_stream;
  const bool synced = phase == SyncPhase::Synced || local;
  const bool down = phase == SyncPhase::Disconnected;
  const bool healthy = health.healthy();
  const bool ready = healthy && (!await_stream || health.order_entry == execution::LinkState::Up);
  switch (state) {
  case NodeState::Syncing:
    if (synced && ready) {
      return LifecycleReason::Synced;
    }
    return down ? std::optional{LifecycleReason::HealthLost} : std::nullopt;
  case NodeState::Running:
    return down || phase == SyncPhase::Buffering || !healthy
               ? std::optional{LifecycleReason::HealthLost}
               : std::nullopt;
  case NodeState::Degraded:
    return healthy && (phase == SyncPhase::Buffering || synced)
               ? std::optional{LifecycleReason::HealthRestored}
               : std::nullopt;
  case NodeState::Init:
  case NodeState::Wired:
  case NodeState::Starting:
  case NodeState::Stopping:
  case NodeState::Stopped:
  case NodeState::Faulted:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace jarvis::engine
