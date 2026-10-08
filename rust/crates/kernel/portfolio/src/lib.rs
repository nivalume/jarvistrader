//! Kernel layer `portfolio` (docs/architecture.md sections 8.5 and 11.2): positions, balances,
//! margin and PnL, updated inside `step` from the fills the OMS accepted, mark prices, funding and
//! account snapshots.
//!
//! - [`position`]: a netting position in integers, with an exact average open price.
//! - [`margin`]: initial and maintenance margin, rounded up onto the currency's grid.
//! - [`portfolio`]: venue positions, the per-strategy ledger, balances, valuation and funding.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

pub mod margin;
pub mod portfolio;
pub mod position;

pub use margin::MarginModel;
pub use portfolio::{
    FillOutcome, FillPart, FundingSettlement, Portfolio, PortfolioConfig, PortfolioStats,
    PositionStep,
};
pub use position::NettingPosition;
