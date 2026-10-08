//! The strict total order of kernel input events.

use crate::time::UnixNanos;

/// Strict total order of kernel input events (docs/architecture.md section 5.2, amending ADR 0001
/// decision 2). In a data source (a catalog log) `seq` is the row within the source; the node's
/// run log renumbers every input it steps with its own ingestion counter, in backtest as in
/// sandbox and live, so no two inputs ever compare equal.
///
/// The derived order is lexicographic in field order: `ts`, then `source_id`, then `seq`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct EventKey {
    pub ts: UnixNanos,
    pub source_id: u16,
    pub seq: u64,
}

impl EventKey {
    #[must_use]
    pub const fn new(ts: UnixNanos, source_id: u16, seq: u64) -> Self {
        Self { ts, source_id, seq }
    }
}
