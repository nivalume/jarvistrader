//! Kernel layer `data` (docs/architecture.md sections 7.2 and 7.5): what sits between an input
//! event and the strategies that want it.
//!
//! - [`intern`]: fixed-capacity interning of instrument ids and bar types into slots, so the hot
//!   path addresses everything by small integers assigned in first-seen order.
//! - [`subscription`]: the subscription matrix (rows are slots, columns are [`DataKind`]s) and
//!   delivery cadences; [`router`] maps an event to its row and kind.
//! - [`book`]: price-level order books indexed by tick, with a read-only view.
//! - [`bars`]: INTERNAL bar aggregation (tick, volume, time).
//! - [`features`]: fixed-point indicators strategies can subscribe to instead of every tick.
//!
//! Everything is sized at construction and nothing allocates afterwards; every state-bearing type
//! has a snapshot encoding (`kernel_core::state`).
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod bars;
pub mod book;
pub mod features;
pub mod intern;
pub mod router;
pub mod subscription;

pub use bars::BarAggregator;
pub use book::{BookLevel, BookView, OrderBook, Side};
pub use features::{FeatureGraph, FeatureId, FeatureKind, FeatureSpec, FeatureValue};
pub use intern::{BarKey, BarTable, InstrumentSlot, InstrumentTable, InternTable};
pub use router::{route_of, Route};
pub use subscription::{
    Cadence, DataKind, Decision, StrategyIndex, Subscriber, SubscriptionMatrix,
};
