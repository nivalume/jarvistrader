//! Clocks. The wall clock decides nothing but the run directory's name; the kernel receives time
//! as the `ts` of its inputs.

#![allow(clippy::disallowed_types)] // this is the one place that reads the clocks

use std::time::{Instant, SystemTime, UNIX_EPOCH};

use kernel_core::time::civil_from_days;
use kernel_core::UnixNanos;

/// The wall clock as Unix nanoseconds; zero before the epoch.
#[must_use]
pub fn wall_now() -> UnixNanos {
    let since = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    UnixNanos::new(u64::try_from(since.as_nanos()).unwrap_or(u64::MAX))
}

/// Elapsed time on the monotonic clock, for telemetry and budgets.
#[derive(Clone, Copy, Debug)]
pub struct Stopwatch {
    start: Instant,
}

impl Default for Stopwatch {
    fn default() -> Self {
        Self::start()
    }
}

impl Stopwatch {
    #[must_use]
    pub fn start() -> Self {
        Self { start: Instant::now() }
    }
    /// Nanoseconds since the start.
    #[must_use]
    pub fn elapsed_ns(&self) -> u64 {
        u64::try_from(self.start.elapsed().as_nanos()).unwrap_or(u64::MAX)
    }
}

/// A run id from a UTC instant: `YYYYmmddTHHMMSSZ`.
#[must_use]
pub fn run_id(t: UnixNanos) -> String {
    let seconds = t.value() / 1_000_000_000;
    let date = civil_from_days((seconds / 86_400) as i64);
    let s = seconds % 86_400;
    format!(
        "{:04}{:02}{:02}T{:02}{:02}{:02}Z",
        date.year,
        date.month,
        date.day,
        s / 3600,
        s / 60 % 60,
        s % 60
    )
}
