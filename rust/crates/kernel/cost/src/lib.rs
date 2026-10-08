//! Kernel layer `cost` (docs/architecture.md section 11.1): the three cost models the matching
//! engine, the execution algorithms and portfolio construction share.
//!
//! - [`fees`]: maker/taker schedules with a discount, and funding; exact integer arithmetic,
//!   rounded onto the currency's grid against the account.
//! - [`slippage`]: the expected cost of taking liquidity, from book depth.
//! - [`latency`]: the simulated venue's delays, pure functions of `(seed, identity, hop)`.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

pub mod fees;
pub mod latency;
pub mod slippage;

pub use fees::{fraction_of, funding, MakerTakerFees, Rate};
pub use latency::{JitteredLatency, LatencyHop};
pub use slippage::{BookDepthSlippage, Level, SlippageEstimate};
