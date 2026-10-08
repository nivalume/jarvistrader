//! Kernel layer `core` (docs/architecture.md section 3): the pieces every other kernel layer is
//! built from. It is the Rust port of `jarvis/core` in the C++ tree, with the same algorithms,
//! the same byte encodings and the same observable results, so the two trees can be compared
//! byte for byte (docs/rust-migration.md section 7).
//!
//! The kernel contract, enforced here by the compiler rather than by the C++ tree's gates:
//!
//! - `no_std`: no clock, no threads, no random state, no I/O can be reached from this crate.
//! - `forbid(unsafe_code)`: ownership and borrowing replace "one owner stores, others borrow";
//!   state that outlives a borrow is addressed by [`Handle`]s into a [`SlotMap`], not pointers.
//! - no floating point: `clippy::float_arithmetic` is denied; values are integers at fixed
//!   scales (ADR 0001, decision 1 as amended by D01).
//! - fallible operations return [`Result<T, Status>`](Status); nothing panics on bad input, only
//!   on violated preconditions that the caller controls (an index past `len`).
//! - capacities are fixed at construction; a full container reports
//!   [`Status::CapacityExceeded`] instead of growing, so nothing allocates after start-up.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod clock;
pub mod crc32c;
pub mod event_key;
pub mod fixed_string;
pub mod fixed_vec;
pub mod int_math;
pub mod priority_queue;
pub mod rng;
pub mod sha256;
pub mod slot_map;
pub mod state;
pub mod status;
pub mod time;

pub use clock::{Clock, FiredTimer, ReplayClock, TimerHandle, TimerKey, TimerQueue, TimerTag};
pub use event_key::EventKey;
pub use fixed_string::FixedString;
pub use fixed_vec::FixedVec;
pub use priority_queue::{Entry, PriorityQueue};
pub use rng::CounterRng;
pub use sha256::Sha256;
pub use slot_map::{Handle, SlotMap};
pub use state::{load_state, save_state, State, StateReader, StateWriter};
pub use status::{Result, Status};
pub use time::{DurationNanos, UnixNanos};
