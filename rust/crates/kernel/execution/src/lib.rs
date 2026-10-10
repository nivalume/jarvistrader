//! Kernel layer `execution` (docs/architecture.md section 8): the order state machine, whose
//! transition table `specs/tla/OrderLifecycle.tla` owns, one order's state under the apply-phase
//! rules, the OMS, and the venue event path. Execution algorithms and reconciliation arrive in a
//! later R3 step (docs/rust-plan.md).
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod apply;
pub mod fsm;
pub mod intent;
pub mod oms;
pub mod order;

pub use apply::{apply_order_event, EventOutcome};
pub use fsm::{is_closed, is_open, is_pending, next_status, OrderEventKind, TRANSITIONS};
pub use intent::OrderIntent;
pub use oms::{Oms, OpenQuantity, OrderIndex, OrderRecord};
pub use order::OrderState;
