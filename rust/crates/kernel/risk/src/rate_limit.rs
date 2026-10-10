//! Order rate limits (docs/architecture.md section 10.4), in the kernel so a backtest throttles
//! exactly like live. Binance counts orders in fixed windows aligned to the clock (10 s and 1 min
//! for USDⓈ-M); a window here holds at most `limit` orders between two multiples of its interval.
//! Time is the input's `ts`, so the state is a function of the inputs and needs no timer. The
//! configured limits sit below the venue's, leaving room for reconciliation and retries.

use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{FixedVec, Status, UnixNanos};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RateWindow {
    pub interval_ns: u64,
    pub limit: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct WindowState {
    window: RateWindow,
    /// Which interval the count belongs to.
    index: u64,
    used: u32,
}

/// Window `i` as of a time, for telemetry.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WindowUse {
    pub interval_ns: u64,
    pub used: u32,
    pub limit: u32,
}

#[derive(Clone, Debug)]
pub struct RateLimiter {
    windows: FixedVec<WindowState>,
}

impl RateLimiter {
    pub const MAX_WINDOWS: usize = 4;

    /// Windows with a zero interval are off; at most [`Self::MAX_WINDOWS`] are kept.
    #[must_use]
    pub fn new(windows: &[RateWindow]) -> Self {
        let mut out = FixedVec::with_capacity(Self::MAX_WINDOWS);
        for w in windows.iter().filter(|w| w.interval_ns > 0).take(Self::MAX_WINDOWS) {
            let _ = out.push(WindowState { window: *w, index: u64::MAX, used: 0 });
        }
        Self { windows: out }
    }

    /// Orders still allowed now (`u32::MAX` without windows).
    pub fn remaining(&mut self, now: UnixNanos) -> u32 {
        let mut left = u32::MAX;
        for i in 0..self.windows.len() {
            let s = self.roll(i, now);
            left = left.min(s.window.limit.saturating_sub(s.used));
        }
        left
    }

    /// Takes `cost` from every window, or nothing when any window lacks room.
    pub fn try_acquire(&mut self, now: UnixNanos, cost: u32) -> bool {
        if self.remaining(now) < cost {
            return false;
        }
        for s in &mut *self.windows {
            s.used += cost;
        }
        true
    }

    /// The venue's count for the window of length `interval_ns` (`RateLimitFeedback`): ours rises
    /// to at least `used`, never falls, so orders the venue counted that we did not (another
    /// session, a retry) are not sent past its limit.
    pub fn feedback(&mut self, now: UnixNanos, interval_ns: u64, used: u32) {
        for i in 0..self.windows.len() {
            if self.windows[i].window.interval_ns == interval_ns {
                let s = self.roll(i, now);
                s.used = s.used.max(used);
            }
        }
    }

    #[must_use]
    pub fn windows(&self) -> usize {
        self.windows.len()
    }

    /// Window `i` as of `now`, without rolling it.
    #[must_use]
    pub fn use_of(&self, i: usize, now: UnixNanos) -> Option<WindowUse> {
        let s = self.windows.get(i)?;
        let current = s.index == now.value() / s.window.interval_ns;
        Some(WindowUse {
            interval_ns: s.window.interval_ns,
            used: if current { s.used } else { 0 },
            limit: s.window.limit,
        })
    }

    fn roll(&mut self, i: usize, now: UnixNanos) -> &mut WindowState {
        let s = &mut self.windows[i];
        let index = now.value() / s.window.interval_ns;
        if index != s.index {
            s.index = index;
            s.used = 0;
        }
        s
    }
}

/// Each window's current interval and count (the windows themselves come from the
/// configuration).
impl State for RateLimiter {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.windows.len() as u32);
        for s in &self.windows {
            w.u64(s.index);
            w.u32(s.used);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        if r.u32() as usize != self.windows.len() {
            r.fail(Status::InvalidState);
            return;
        }
        for s in &mut *self.windows {
            s.index = r.u64();
            s.used = r.u32();
        }
    }
}
