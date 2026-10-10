//! Kernel layer `risk` (docs/architecture.md sections 9 and 10): the structural order checks, the
//! rule catalog behind Gate A and Gate B, `TradingState` (whose tables `specs/tla/TradingState.tla`
//! owns) and the order rate limit. The post-trade monitors and the kill switch arrive in a later
//! R3 step (docs/rust-plan.md).
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod checks;
pub mod gates;
pub mod rate_limit;
pub mod trading_state;

pub use checks::{check_intent, ReasonCode};
pub use gates::{MarginCheck, OrderCheck, RiskConfig, RiskEngine, RiskStats};
pub use rate_limit::{RateLimiter, RateWindow};
pub use trading_state::{allowed, CommandKind, TradingStateMachine, TradingTrigger, MATRIX};
