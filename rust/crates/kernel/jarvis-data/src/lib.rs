//! Routing, order books, bars and features.
//!
//! Kernel layer `data` (docs/architecture.md section 3). Not yet ported: the C++ tree under
//! `jarvis/data` is the reference, and docs/rust-plan.md names the milestone that fills this crate.
//! The crate attributes below are the kernel contract and stay as they are when code lands.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]
