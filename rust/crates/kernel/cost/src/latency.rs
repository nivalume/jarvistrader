//! Delays of the simulated venue (docs/architecture.md sections 11.1 and 12.2). A delay is a pure
//! function of `(seed, identity, hop, attempt)`: the identity is the input's `seq` for market data
//! and a hash of the `ClientOrderId` for commands, so a replay draws the same delays.

use kernel_core::rng::{mix64, CounterRng};
use kernel_core::{DurationNanos, Result, Status};

/// Where in the round trip the delay applies.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum LatencyHop {
    /// Venue event to the strategy's observation (`ts_init = ts_event + feed`).
    Feed = 1,
    /// A command leaving the strategy to the venue.
    Outbound = 2,
    /// A venue report back to the strategy.
    Inbound = 3,
}

impl LatencyHop {
    pub const ALL: [LatencyHop; 3] = [LatencyHop::Feed, LatencyHop::Outbound, LatencyHop::Inbound];
}

/// A fixed base per hop plus a uniform jitter in `[0, jitter]`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct JitteredLatency {
    rng: CounterRng,
    base: [DurationNanos; 3],
    jitter: [DurationNanos; 3],
}

impl JitteredLatency {
    /// Latency draws use their own Philox key, derived from the node seed, apart from the other
    /// kernel draws.
    #[must_use]
    pub fn new(seed: u64, base: [DurationNanos; 3], jitter: [DurationNanos; 3]) -> Self {
        Self { rng: CounterRng::new(mix64(seed ^ 0x4C41_5445_4E43_5900)), base, jitter }
    }

    /// The same delay on every hop and no jitter: a deterministic venue for tests.
    #[must_use]
    pub fn constant(seed: u64, delay: DurationNanos) -> Self {
        Self::new(seed, [delay; 3], [DurationNanos::default(); 3])
    }

    /// The delay of `identity` on `hop`; `attempt` separates several delays of one identity on
    /// one hop (the n-th answer to one order).
    #[must_use]
    pub fn delay(&self, identity: u64, hop: LatencyHop, attempt: u32) -> DurationNanos {
        let i = hop as usize - 1;
        let jitter = self.jitter[i].value();
        let extra =
            if jitter == 0 { 0 } else { self.rng.below(jitter + 1, identity, hop as u32, attempt) };
        DurationNanos::new(self.base[i].value().saturating_add(extra))
    }

    /// Checks the sum of base and jitter fits a `u64` on every hop.
    pub fn validate(&self) -> Result<()> {
        for i in 0..3 {
            self.base[i].value().checked_add(self.jitter[i].value()).ok_or(Status::Overflow)?;
        }
        Ok(())
    }
}
