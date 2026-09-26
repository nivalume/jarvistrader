#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/event.hpp"

// Node lifecycle state machine (docs/architecture.md section 4.4). A pure transition function:
// the core thread asks it for the next state, records the NodeLifecycle event it returns, and
// only then acts on the new state, so replaying the log reproduces every transition.
//
//   Init -> Wired -> Starting -> Syncing -> Running <-> Degraded -> Stopping -> Stopped
//   and any non-terminal state -> Faulted.

namespace jarvis::engine {

using model::LifecycleReason;
using model::NodeState;

inline constexpr std::size_t kNodeStateCount = 9;
inline constexpr std::size_t kLifecycleReasonCount = 10;

struct LifecycleTransition {
  NodeState from;
  LifecycleReason reason;
  NodeState to;
};

// Every allowed transition. Anything else is InvalidTransition.
inline constexpr std::array<LifecycleTransition, 22> kLifecycleTransitions = {{
    {NodeState::Init, LifecycleReason::Configured, NodeState::Wired},
    {NodeState::Wired, LifecycleReason::RunRequested, NodeState::Starting},
    {NodeState::Starting, LifecycleReason::Started, NodeState::Syncing},
    {NodeState::Syncing, LifecycleReason::Synced, NodeState::Running},
    {NodeState::Syncing, LifecycleReason::HealthLost, NodeState::Degraded},
    {NodeState::Running, LifecycleReason::HealthLost, NodeState::Degraded},
    {NodeState::Degraded, LifecycleReason::HealthRestored, NodeState::Syncing},
    {NodeState::Running, LifecycleReason::EndOfData, NodeState::Stopping},
    {NodeState::Wired, LifecycleReason::ShutdownRequested, NodeState::Stopping},
    {NodeState::Starting, LifecycleReason::ShutdownRequested, NodeState::Stopping},
    {NodeState::Syncing, LifecycleReason::ShutdownRequested, NodeState::Stopping},
    {NodeState::Running, LifecycleReason::ShutdownRequested, NodeState::Stopping},
    {NodeState::Degraded, LifecycleReason::ShutdownRequested, NodeState::Stopping},
    {NodeState::Stopping, LifecycleReason::Drained, NodeState::Stopped},
    {NodeState::Stopping, LifecycleReason::ShutdownRequested, NodeState::Stopping},
    {NodeState::Init, LifecycleReason::Fault, NodeState::Faulted},
    {NodeState::Wired, LifecycleReason::Fault, NodeState::Faulted},
    {NodeState::Starting, LifecycleReason::Fault, NodeState::Faulted},
    {NodeState::Syncing, LifecycleReason::Fault, NodeState::Faulted},
    {NodeState::Running, LifecycleReason::Fault, NodeState::Faulted},
    {NodeState::Degraded, LifecycleReason::Fault, NodeState::Faulted},
    {NodeState::Stopping, LifecycleReason::Fault, NodeState::Faulted},
    // Stopped and Faulted are terminal: no transition leaves them.
}};

[[nodiscard]] constexpr bool is_terminal(NodeState s) noexcept {
  return s == NodeState::Stopped || s == NodeState::Faulted;
}

// Only Running accepts market data and strategy callbacks; Syncing and Degraded accept venue
// events (reconciliation) but no new strategy commands.
[[nodiscard]] constexpr bool strategies_active(NodeState s) noexcept {
  return s == NodeState::Running;
}

// The state reached from `from` for `reason`, or InvalidTransition. A repeated shutdown request
// while Stopping is accepted and keeps the state, so signal handlers need no deduplication.
[[nodiscard]] constexpr core::Status next_state(NodeState from, LifecycleReason reason,
                                                NodeState& to) noexcept {
  for (const LifecycleTransition& t : kLifecycleTransitions) {
    if (t.from == from && t.reason == reason) {
      to = t.to;
      return core::Status::Ok;
    }
  }
  return core::Status::InvalidTransition;
}

// The machine plus the event that records each transition.
class Lifecycle {
public:
  [[nodiscard]] constexpr NodeState state() const noexcept { return state_; }

  // Moves to the next state and fills `event` with the NodeLifecycle record to log.
  [[nodiscard]] constexpr core::Status apply(LifecycleReason reason, core::UnixNanos ts_init,
                                             model::NodeLifecycle& event) noexcept {
    NodeState to = state_;
    const core::Status s = next_state(state_, reason, to);
    if (!core::ok(s)) {
      return s;
    }
    event = model::NodeLifecycle{state_, to, reason, ts_init};
    state_ = to;
    return core::Status::Ok;
  }

  // Replay: applies a recorded transition, which must match what the machine would do.
  [[nodiscard]] constexpr core::Status replay(const model::NodeLifecycle& event) noexcept {
    NodeState to = state_;
    if (event.from != state_ || !core::ok(next_state(state_, event.reason, to)) || to != event.to) {
      return core::Status::InvalidTransition;
    }
    state_ = to;
    return core::Status::Ok;
  }

private:
  NodeState state_ = NodeState::Init;
};

} // namespace jarvis::engine
