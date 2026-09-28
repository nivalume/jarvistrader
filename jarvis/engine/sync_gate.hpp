#pragma once

#include <optional>

#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/execution/reconciliation.hpp"

// The driver's sync gate (docs/architecture.md sections 4.4 and 15.1): the lifecycle move the
// account's reconciliation phase calls for. The driver applies it after every input until it
// returns nothing, recording each transition, so a replay needs no gate of its own.
//
//   Syncing   -> Running    once the account is synced (with `await_stream`, a node that has
//                           never heard from the user data stream is not: live waits for it);
//   Syncing   -> Degraded   the stream dropped while reconciling;
//   Running   -> Degraded   the stream dropped (the kernel halted trading in the same step);
//   Degraded  -> Syncing    the stream is back: reconciliation runs again.
//
// Without `await_stream` (backtest, sandbox) a node without a user stream counts as synced, so
// the gate never moves it.

namespace jarvis::engine {

[[nodiscard]] constexpr std::optional<LifecycleReason>
sync_move(NodeState state, execution::SyncPhase phase, bool await_stream) noexcept {
  using execution::SyncPhase;
  const bool synced = phase == SyncPhase::Synced || (phase == SyncPhase::Local && !await_stream);
  const bool down = phase == SyncPhase::Disconnected;
  switch (state) {
  case NodeState::Syncing:
    if (synced) {
      return LifecycleReason::Synced;
    }
    return down ? std::optional{LifecycleReason::HealthLost} : std::nullopt;
  case NodeState::Running:
    return down || phase == SyncPhase::Buffering ? std::optional{LifecycleReason::HealthLost}
                                                 : std::nullopt;
  case NodeState::Degraded:
    return phase == SyncPhase::Buffering || phase == SyncPhase::Synced
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
