//! Clocks and the deterministic timer queue.

use crate::event_key::EventKey;
use crate::priority_queue::{Entry, PriorityQueue};
use crate::slot_map::{Handle, SlotMap};
use crate::status::{Result, Status};
use crate::time::{DurationNanos, UnixNanos};

/// Something that tells the time. The kernel never reads a clock of its own; the node hands it
/// one of these, a [`ReplayClock`] in backtest and the shell's monotonic clock live.
pub trait Clock {
    fn now(&self) -> UnixNanos;
}

/// Clock of a backtest: time moves only when the engine advances it to the next event.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReplayClock {
    now: UnixNanos,
}

impl ReplayClock {
    #[must_use]
    pub const fn new(start: UnixNanos) -> Self {
        Self { now: start }
    }

    /// Time never goes backwards.
    pub fn advance_to(&mut self, t: UnixNanos) -> Result<()> {
        if t < self.now {
            return Err(Status::InvalidArgument);
        }
        self.now = t;
        Ok(())
    }
}

impl Clock for ReplayClock {
    fn now(&self) -> UnixNanos {
        self.now
    }
}

/// Caller-chosen identity of a timer: the owner (a strategy index, an algorithm) and an id.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct TimerKey {
    pub owner: u32,
    pub id: u32,
}

impl TimerKey {
    #[must_use]
    pub const fn new(owner: u32, id: u32) -> Self {
        Self { owner, id }
    }
}

/// The handle type of timers.
#[derive(Debug)]
pub enum TimerTag {}
pub type TimerHandle = Handle<TimerTag>;

/// A timer that came due.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FiredTimer {
    pub handle: TimerHandle,
    pub key: TimerKey,
    pub deadline: UnixNanos,
}

/// A scheduled timer. `armed_seq` is the heap entry that currently represents it; older entries
/// for the same handle are stale.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Timer {
    pub(crate) key: TimerKey,
    pub(crate) period: DurationNanos,
    pub(crate) armed_seq: u64,
}

/// Deterministic timer queue. Timers fire in (deadline, schedule order); a periodic timer is
/// re-armed at `deadline + period` under the same handle. Cancelling leaves a stale heap entry,
/// skipped on pop and purged in place when the heap fills, so nothing allocates after
/// construction.
#[derive(Clone, Debug)]
pub struct TimerQueue {
    pub(crate) timers: SlotMap<Timer, TimerTag>,
    pub(crate) heap: PriorityQueue<TimerHandle>,
    pub(crate) seq: u64,
}

impl TimerQueue {
    /// Room for `capacity` live timers; the heap holds twice that, so stale entries rarely force a
    /// purge.
    #[must_use]
    pub fn with_capacity(capacity: u32) -> Self {
        Self {
            timers: SlotMap::with_capacity(capacity),
            heap: PriorityQueue::with_capacity(capacity as usize * 2),
            seq: 0,
        }
    }

    /// A one-shot timer when `period` is zero, otherwise periodic.
    pub fn schedule(
        &mut self,
        deadline: UnixNanos,
        period: DurationNanos,
        key: TimerKey,
    ) -> Result<TimerHandle> {
        let handle = self.timers.insert(Timer { key, period, armed_seq: 0 })?;
        if let Err(e) = self.arm(handle, deadline) {
            let _ = self.timers.remove(handle);
            return Err(e);
        }
        Ok(handle)
    }

    pub fn cancel(&mut self, handle: TimerHandle) -> Result<()> {
        self.timers.remove(handle).map(drop)
    }

    /// Pops the next live timer whose deadline is at or before `now`.
    pub fn pop_due(&mut self, now: UnixNanos) -> Option<FiredTimer> {
        loop {
            let top = self.heap.peek()?;
            if top.key.ts > now {
                return None;
            }
            let entry = self.heap.pop()?;
            let handle = entry.payload;
            let Some(timer) = self.live(&entry).then(|| self.timers[handle]) else {
                continue; // cancelled, or superseded by a re-arm
            };
            let fired = FiredTimer { handle, key: timer.key, deadline: entry.key.ts };
            let rearmed = if timer.period.is_zero() {
                Err(Status::InvalidState) // one-shot: done
            } else {
                entry.key.ts.plus(timer.period).and_then(|next| self.arm(handle, next))
            };
            if rearmed.is_err() {
                let _ = self.timers.remove(handle);
            }
            return Some(fired);
        }
    }

    /// Pops the stale entries off the top of the heap, so that the top is a live timer. The engine
    /// calls it at the end of every step: between steps `peek()` then finds the next timer at once.
    pub fn prune(&mut self) {
        while self.heap.peek().is_some_and(|top| !self.live(top)) {
            self.heap.pop();
        }
    }

    /// The next live timer (earliest deadline, then schedule order) without firing it. It changes
    /// nothing, not even the stale entries: what a node's driver asks between steps must leave the
    /// state as a replay, which does not ask, has it (snapshots compare it). After [`prune`] the
    /// answer is the top entry.
    ///
    /// [`prune`]: TimerQueue::prune
    #[must_use]
    pub fn peek(&self) -> Option<FiredTimer> {
        let next = self.heap.min_where(|e| self.live(e))?;
        let timer = self.timers.get(next.payload)?;
        Some(FiredTimer { handle: next.payload, key: timer.key, deadline: next.key.ts })
    }

    #[must_use]
    pub fn next_deadline(&self) -> Option<UnixNanos> {
        self.peek().map(|t| t.deadline)
    }

    /// Live timers.
    #[must_use]
    pub fn active(&self) -> usize {
        self.timers.len()
    }

    fn live(&self, entry: &Entry<TimerHandle>) -> bool {
        self.timers.get(entry.payload).is_some_and(|t| t.armed_seq == entry.key.seq)
    }

    fn arm(&mut self, handle: TimerHandle, deadline: UnixNanos) -> Result<()> {
        if self.heap.is_full() {
            let timers = &self.timers;
            self.heap.retain(|e| timers.get(e.payload).is_some_and(|t| t.armed_seq == e.key.seq));
        }
        self.seq += 1;
        let seq = self.seq;
        self.timers.get_mut(handle).ok_or(Status::NotFound)?.armed_seq = seq;
        self.heap.push(EventKey::new(deadline, 0, seq), handle)
    }
}
