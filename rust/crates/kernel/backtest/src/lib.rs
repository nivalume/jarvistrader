//! Kernel layer `backtest` (docs/architecture.md section 12): event sources (recorded logs, merged
//! streams), the simulated exchange and its fill models, the two timelines of the venue loop, the
//! step loop that turns data into the node's input sequence, and the replay of a run log.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod driver;
pub mod replay;
pub mod sim;
pub mod source;
pub mod venue_loop;

pub use driver::{
    Driver, DriverOptions, FingerprintRecorder, LogRecorder, Recorder, RunSummary, Source,
};
pub use replay::{replay, restore, ReplayError, ReplayReport};
pub use sim::{FillModel, SimConfig, SimOrder, SimStats, SimulatedExchange, StpMode};
pub use source::{EventSource, MergeSource, ReplaySource, VecSource};
pub use venue_loop::{VenueLoop, VenueLoopConfig, VenueLoopStats, VENUE_SOURCE};
