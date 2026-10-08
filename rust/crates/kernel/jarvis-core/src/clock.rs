//! Clocks and the deterministic timer queue.

use crate::arena::{Arena, Handle};
use crate::event_key::EventKey;
use crate::priority_queue::{Entry, PriorityQueue};
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

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct TimerSlot {
    pub(crate) key: TimerKey,
    pub(crate) period: DurationNanos,
    pub(crate) armed_seq: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct HeapEntry {
    pub(crate) handle: TimerHandle,
}

/// Deterministic timer queue. Timers fire in (deadline, schedule order); a periodic timer is
/// re-armed at `deadline + period` under the same handle. Cancelled timers leave stale heap entries
/// that are skipped on pop and purged in place when the heap fills, so nothing allocates after
/// construction.
#[derive(Clone, Debug)]
pub struct TimerQueue {
    pub(crate) slots: Arena<TimerSlot, TimerTag>,
    pub(crate) heap: PriorityQueue<HeapEntry>,
    pub(crate) seq: u64,
}

impl TimerQueue {
    #[must_use]
    pub fn with_capacity(capacity: u32) -> Self {
        Self {
            slots: Arena::with_capacity(capacity),
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
        if self.heap.is_full() {
            self.purge();
        }
        let handle = self.slots.insert(TimerSlot { key, period, armed_seq: 0 })?;
        if let Err(s) = self.arm(handle, deadline) {
            let _ = self.slots.erase(handle);
            return Err(s);
        }
        Ok(handle)
    }

    pub fn cancel(&mut self, handle: TimerHandle) -> Result<()> {
        self.slots.erase(handle)
    }

    /// Pops the next live timer whose deadline is at or before `now`.
    pub fn pop_due(&mut self, now: UnixNanos) -> Option<FiredTimer> {
        while let Some(top) = self.heap.peek() {
            if top.key.ts > now {
                return None;
            }
            let entry = self.heap.pop()?;
            let handle = entry.payload.handle;
            let Some(slot) = self.slots.get(handle) else { continue };
            if slot.armed_seq != entry.key.seq {
                continue; // cancelled, or superseded by a re-arm
            }
            let fired = FiredTimer { handle, key: slot.key, deadline: entry.key.ts };
            let period = slot.period;
            if period.is_zero() {
                let _ = self.slots.erase(handle);
            } else {
                let rearmed = match entry.key.ts.plus(period) {
                    Ok(next) => self.arm(handle, next),
                    Err(s) => Err(s),
                };
                if rearmed.is_err() {
                    let _ = self.slots.erase(handle);
                }
            }
            return Some(fired);
        }
        None
    }

    /// Pops the stale entries off the top of the heap, so that the top is a live timer. The engine
    /// calls it at the end of every step: between steps `peek()` then finds the next timer at once.
    pub fn prune(&mut self) {
        while let Some(top) = self.heap.peek() {
            if self.is_live(top) {
                return;
            }
            let _ = self.heap.pop();
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
        let next = self.heap.min_where(|e| self.is_live(e))?;
        let slot = self.slots.get(next.payload.handle)?;
        Some(FiredTimer { handle: next.payload.handle, key: slot.key, deadline: next.key.ts })
    }

    #[must_use]
    pub fn next_deadline(&self) -> Option<UnixNanos> {
        self.peek().map(|t| t.deadline)
    }

    /// Live timers.
    #[must_use]
    pub fn active(&self) -> usize {
        self.slots.len()
    }

    fn is_live(&self, e: &Entry<HeapEntry>) -> bool {
        match self.slots.get(e.payload.handle) {
            Some(slot) => slot.armed_seq == e.key.seq,
            None => false,
        }
    }

    fn arm(&mut self, handle: TimerHandle, deadline: UnixNanos) -> Result<()> {
        if self.heap.is_full() {
            self.purge();
        }
        self.seq += 1;
        let seq = self.seq;
        let slot = self.slots.get_mut(handle).ok_or(Status::NotFound)?;
        slot.armed_seq = seq;
        self.heap.push(EventKey::new(deadline, 0, seq), HeapEntry { handle })
    }

    fn purge(&mut self) {
        let slots = &self.slots;
        self.heap.retain(|e| match slots.get(e.payload.handle) {
            Some(slot) => slot.armed_seq == e.key.seq,
            None => false,
        });
    }
}
