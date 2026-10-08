//! Kernel layer `model` (docs/architecture.md section 3 and 6): the domain model, aligned with
//! `nautilus_trader` `crates/model` at `cd417b80` as a compatibility contract (identifiers and
//! their string formats, fixed-point values on the 1e9 scale, enums with nautilus's integers and
//! strings, data and event types with the same fields), and jarvis's own additions (the
//! `ClientOrderId` format, the closed [`Event`] enum, the event log wire format and fingerprints).
//!
//! The contract is checked by `tests/conformance.rs` against vectors generated from
//! `tests/conformance/nautilus_cd417b80.json`; the encodings are checked by property tests
//! (decode after encode is the identity, encode after decode reproduces the bytes) over the
//! `corpus` crate's deterministic corpus, and by the determinism gate, which fingerprints that
//! corpus in two build profiles.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod account;
pub mod bar;
pub mod client_order_id;
pub mod currency;
pub mod data;
pub mod decimal;
pub mod enums;
pub mod event;
pub mod fixed_point;
pub mod identifiers;
pub mod instruments;
pub mod log;
pub mod money;
pub mod order_events;
pub mod position_events;
pub mod uuid;
pub mod wire;

mod generated {
    pub(crate) mod currencies;
}

pub use currency::{Currency, CurrencyType};
pub use event::{Event, EventKind};
pub use fixed_point::{Price, Quantity, FIXED_PRECISION, FIXED_SCALAR};
pub use identifiers::{
    AccountId, ClientOrderId, InstrumentId, PositionId, StrategyId, Symbol, TradeId, TraderId,
    Venue, VenueOrderId,
};
pub use money::Money;
pub use uuid::Uuid4;
pub use wire::{Wire, WireReader, WireWriter};
