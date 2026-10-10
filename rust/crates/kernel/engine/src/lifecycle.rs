//! The node lifecycle state machine (docs/architecture.md section 4.4): a pure function
//! `next_state(from, reason)`. Every transition is recorded as a `NodeLifecycle` input, so a replay
//! reproduces it; `specs/tla/NodeLifecycle.tla` models the node around it.

use kernel_core::{Result, Status, UnixNanos};
use model::enums::{LifecycleReason, NodeState};
use model::event::NodeLifecycle;

/// The state `reason` moves `from` to, or `InvalidTransition`.
pub fn next_state(from: NodeState, reason: LifecycleReason) -> Result<NodeState> {
    use LifecycleReason as R;
    use NodeState as S;
    Ok(match (from, reason) {
        (S::Init, R::Configured) => S::Wired,
        (S::Wired, R::RunRequested) => S::Starting,
        (S::Starting, R::Started) | (S::Degraded, R::HealthRestored) => S::Syncing,
        (S::Syncing, R::Synced) => S::Running,
        (S::Syncing | S::Running, R::HealthLost) => S::Degraded,
        (S::Running, R::EndOfData)
        | (
            S::Wired | S::Starting | S::Syncing | S::Running | S::Degraded | S::Stopping,
            R::ShutdownRequested,
        ) => S::Stopping,
        (S::Stopping, R::Drained) => S::Stopped,
        (S::Stopped | S::Faulted, R::Fault) => return Err(Status::InvalidTransition),
        (_, R::Fault) => S::Faulted,
        _ => return Err(Status::InvalidTransition),
    })
}

#[must_use]
pub const fn is_terminal(state: NodeState) -> bool {
    matches!(state, NodeState::Stopped | NodeState::Faulted)
}

/// The node's current state and the transitions it records.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Lifecycle {
    state: NodeState,
}

impl Lifecycle {
    #[must_use]
    pub const fn state(&self) -> NodeState {
        self.state
    }

    /// Applies `reason`; the transition to record.
    pub fn apply(&mut self, reason: LifecycleReason, ts: UnixNanos) -> Result<NodeLifecycle> {
        let to = next_state(self.state, reason)?;
        let event = NodeLifecycle { from: self.state, to, reason, ts };
        self.state = to;
        Ok(event)
    }
}
