//! When a node takes an `EngineState` snapshot (docs/architecture.md section 16.3): right after
//! stepping a `BatchEnd` input, once at least `every` inputs have been stepped since the last
//! snapshot point (the first point counts from the start), or once a snapshot was requested.
//! Taken there, the state is between two batches with every output flushed; replay applies the
//! same rule to the recorded inputs, so both take their snapshots after the same inputs.

use model::enums::NodeState;
use model::Event;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SnapshotSchedule {
    every: u64,
    next: u64,
    requested: bool,
}

impl SnapshotSchedule {
    /// `every` 0: never.
    #[must_use]
    pub const fn new(every: u64) -> Self {
        Self { every, next: every, requested: false }
    }

    /// After the step of input `seq`; true when a snapshot is taken now.
    pub fn due(&mut self, seq: u64, batch_end: bool) -> bool {
        if !batch_end || (!self.requested && (self.every == 0 || seq < self.next)) {
            return false;
        }
        self.next = seq.saturating_add(self.every);
        self.requested = false;
        true
    }

    /// One snapshot at the next batch end.
    pub fn request(&mut self) {
        self.requested = true;
    }

    /// Continues from a snapshot taken after input `seq`.
    pub fn resume_after(&mut self, seq: u64) {
        self.next = seq.saturating_add(self.every);
    }

    #[must_use]
    pub const fn every(&self) -> u64 {
        self.every
    }
}

/// Whether the input just stepped asks for a snapshot at the next batch end: the `Stopped`
/// transition of a node that stepped a `Shutdown` input (`shut_down`), the final snapshot of a
/// sandbox or live run from which a resumed run continues. A backtest records no `Shutdown`.
#[must_use]
pub fn asks_for_snapshot(event: &Event, shut_down: bool) -> bool {
    match event {
        Event::NodeLifecycle(lc) => shut_down && lc.to == NodeState::Stopped,
        _ => false,
    }
}
