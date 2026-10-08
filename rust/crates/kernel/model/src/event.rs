//! The closed set of kernel inputs (docs/architecture.md section 5.1). Adding a kind means adding
//! a variant here, a tag in [`EventKind`], and bumping [`SCHEMA_VERSION`]; every `match` on the
//! enum is then checked for exhaustiveness by the compiler.

use kernel_core::clock::TimerKey;
use kernel_core::{Result, Status, UnixNanos};

use crate::account::AccountState;
use crate::bar::Bar;
use crate::data::{
    FundingRateUpdate, IndexPriceUpdate, InstrumentClose, InstrumentStatus, LiquidationOrder,
    MarkPriceUpdate, OrderBookDeltas, QuoteTick, TradeTick,
};
use crate::enums::StopMode;
use crate::instruments::Instrument;
use crate::order_events::OrderEvent;
use crate::wire::{Wire, WireReader, WireWriter};
use crate::wire_struct;

/// The version of the set of kinds and their encodings. It goes into the log header.
pub const SCHEMA_VERSION: u16 = 1;

/// A timer that came due, recorded as an input so a replay fires it at the same point.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TimerFired {
    pub key: TimerKey,
    pub deadline: UnixNanos,
}
wire_struct!(TimerFired { key, deadline });

/// The end of a batch of inputs with the same `ts`: the window of `Conflated` subscriptions.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct BatchEnd {
    pub ts: UnixNanos,
}
wire_struct!(BatchEnd { ts });

/// A request to stop, from a signal or the admin channel.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Shutdown {
    pub mode: StopMode,
}
wire_struct!(Shutdown { mode });

/// Rate-limit headers the venue returned with a response (section 10.4).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct RateLimitFeedback {
    pub used_weight_1m: u32,
    pub order_count_10s: u32,
    pub order_count_1m: u32,
    pub ts_init: UnixNanos,
}
wire_struct!(RateLimitFeedback { used_weight_1m, order_count_10s, order_count_1m, ts_init });

/// Every input the kernel's `step` takes. Market data, venue, reference, time and control events;
/// the kernel's own recorded outputs (`NodeLifecycle`, `StrategyError`) join when their layers
/// land (docs/rust-plan.md R3, R4).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    TradeTick(TradeTick),
    QuoteTick(QuoteTick),
    OrderBookDeltas(OrderBookDeltas),
    Bar(Bar),
    MarkPriceUpdate(MarkPriceUpdate),
    IndexPriceUpdate(IndexPriceUpdate),
    FundingRateUpdate(FundingRateUpdate),
    InstrumentStatus(InstrumentStatus),
    InstrumentClose(InstrumentClose),
    LiquidationOrder(LiquidationOrder),
    Instrument(Instrument),
    Order(OrderEvent),
    AccountState(AccountState),
    RateLimitFeedback(RateLimitFeedback),
    TimerFired(TimerFired),
    BatchEnd(BatchEnd),
    Shutdown(Shutdown),
}

/// The tag of an [`Event`] on the wire, and its category.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u16)]
pub enum EventKind {
    TradeTick = 1,
    QuoteTick = 2,
    OrderBookDeltas = 3,
    Bar = 4,
    MarkPriceUpdate = 5,
    IndexPriceUpdate = 6,
    FundingRateUpdate = 7,
    InstrumentStatus = 8,
    InstrumentClose = 9,
    LiquidationOrder = 10,
    Instrument = 20,
    Order = 30,
    AccountState = 31,
    RateLimitFeedback = 32,
    TimerFired = 40,
    BatchEnd = 41,
    Shutdown = 50,
}

/// The categories of section 5.1.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EventCategory {
    MarketData,
    Reference,
    Venue,
    Time,
    Control,
}

impl EventKind {
    pub const ALL: &'static [EventKind] = &[
        EventKind::TradeTick,
        EventKind::QuoteTick,
        EventKind::OrderBookDeltas,
        EventKind::Bar,
        EventKind::MarkPriceUpdate,
        EventKind::IndexPriceUpdate,
        EventKind::FundingRateUpdate,
        EventKind::InstrumentStatus,
        EventKind::InstrumentClose,
        EventKind::LiquidationOrder,
        EventKind::Instrument,
        EventKind::Order,
        EventKind::AccountState,
        EventKind::RateLimitFeedback,
        EventKind::TimerFired,
        EventKind::BatchEnd,
        EventKind::Shutdown,
    ];

    pub const fn from_tag(tag: u16) -> Result<Self> {
        let mut i = 0;
        while i < Self::ALL.len() {
            if Self::ALL[i] as u16 == tag {
                return Ok(Self::ALL[i]);
            }
            i += 1;
        }
        Err(Status::UnsupportedMessage)
    }

    #[must_use]
    pub const fn tag(self) -> u16 {
        self as u16
    }

    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            EventKind::TradeTick => "TradeTick",
            EventKind::QuoteTick => "QuoteTick",
            EventKind::OrderBookDeltas => "OrderBookDeltas",
            EventKind::Bar => "Bar",
            EventKind::MarkPriceUpdate => "MarkPriceUpdate",
            EventKind::IndexPriceUpdate => "IndexPriceUpdate",
            EventKind::FundingRateUpdate => "FundingRateUpdate",
            EventKind::InstrumentStatus => "InstrumentStatus",
            EventKind::InstrumentClose => "InstrumentClose",
            EventKind::LiquidationOrder => "LiquidationOrder",
            EventKind::Instrument => "Instrument",
            EventKind::Order => "Order",
            EventKind::AccountState => "AccountState",
            EventKind::RateLimitFeedback => "RateLimitFeedback",
            EventKind::TimerFired => "TimerFired",
            EventKind::BatchEnd => "BatchEnd",
            EventKind::Shutdown => "Shutdown",
        }
    }

    #[must_use]
    pub const fn category(self) -> EventCategory {
        match self {
            EventKind::TradeTick
            | EventKind::QuoteTick
            | EventKind::OrderBookDeltas
            | EventKind::Bar
            | EventKind::MarkPriceUpdate
            | EventKind::IndexPriceUpdate
            | EventKind::FundingRateUpdate
            | EventKind::InstrumentStatus
            | EventKind::InstrumentClose
            | EventKind::LiquidationOrder => EventCategory::MarketData,
            EventKind::Instrument => EventCategory::Reference,
            EventKind::Order | EventKind::AccountState | EventKind::RateLimitFeedback => {
                EventCategory::Venue
            }
            EventKind::TimerFired | EventKind::BatchEnd => EventCategory::Time,
            EventKind::Shutdown => EventCategory::Control,
        }
    }
}

impl Event {
    #[must_use]
    pub const fn kind(&self) -> EventKind {
        match self {
            Event::TradeTick(_) => EventKind::TradeTick,
            Event::QuoteTick(_) => EventKind::QuoteTick,
            Event::OrderBookDeltas(_) => EventKind::OrderBookDeltas,
            Event::Bar(_) => EventKind::Bar,
            Event::MarkPriceUpdate(_) => EventKind::MarkPriceUpdate,
            Event::IndexPriceUpdate(_) => EventKind::IndexPriceUpdate,
            Event::FundingRateUpdate(_) => EventKind::FundingRateUpdate,
            Event::InstrumentStatus(_) => EventKind::InstrumentStatus,
            Event::InstrumentClose(_) => EventKind::InstrumentClose,
            Event::LiquidationOrder(_) => EventKind::LiquidationOrder,
            Event::Instrument(_) => EventKind::Instrument,
            Event::Order(_) => EventKind::Order,
            Event::AccountState(_) => EventKind::AccountState,
            Event::RateLimitFeedback(_) => EventKind::RateLimitFeedback,
            Event::TimerFired(_) => EventKind::TimerFired,
            Event::BatchEnd(_) => EventKind::BatchEnd,
            Event::Shutdown(_) => EventKind::Shutdown,
        }
    }

    /// `ts_init` of a timestamped event: when it was created locally. Timer and batch events are
    /// at their own instant; a shutdown has none.
    #[must_use]
    pub const fn ts_init(&self) -> Option<UnixNanos> {
        Some(match self {
            Event::TradeTick(e) => e.ts_init,
            Event::QuoteTick(e) => e.ts_init,
            Event::OrderBookDeltas(e) => e.ts_init,
            Event::Bar(e) => e.ts_init,
            Event::MarkPriceUpdate(e) => e.ts_init,
            Event::IndexPriceUpdate(e) => e.ts_init,
            Event::FundingRateUpdate(e) => e.ts_init,
            Event::InstrumentStatus(e) => e.ts_init,
            Event::InstrumentClose(e) => e.ts_init,
            Event::LiquidationOrder(e) => e.ts_init,
            Event::Instrument(e) => e.ts_init,
            Event::Order(e) => e.header().ts_init,
            Event::AccountState(e) => e.ts_init,
            Event::RateLimitFeedback(e) => e.ts_init,
            Event::TimerFired(e) => e.deadline,
            Event::BatchEnd(e) => e.ts,
            Event::Shutdown(_) => return None,
        })
    }

    /// Encodes the body (without the kind tag; the log record carries it).
    pub fn encode_body(&self, w: &mut WireWriter) {
        match self {
            Event::TradeTick(e) => w.put(e),
            Event::QuoteTick(e) => w.put(e),
            Event::OrderBookDeltas(e) => w.put(e),
            Event::Bar(e) => w.put(e),
            Event::MarkPriceUpdate(e) => w.put(e),
            Event::IndexPriceUpdate(e) => w.put(e),
            Event::FundingRateUpdate(e) => w.put(e),
            Event::InstrumentStatus(e) => w.put(e),
            Event::InstrumentClose(e) => w.put(e),
            Event::LiquidationOrder(e) => w.put(e),
            Event::Instrument(e) => w.put(e),
            Event::Order(e) => w.put(e),
            Event::AccountState(e) => w.put(e),
            Event::RateLimitFeedback(e) => w.put(e),
            Event::TimerFired(e) => w.put(e),
            Event::BatchEnd(e) => w.put(e),
            Event::Shutdown(e) => w.put(e),
        }
    }

    /// Decodes a body of the given kind.
    pub fn decode_body(kind: EventKind, r: &mut WireReader<'_>) -> Result<Self> {
        Ok(match kind {
            EventKind::TradeTick => Event::TradeTick(r.get()?),
            EventKind::QuoteTick => Event::QuoteTick(r.get()?),
            EventKind::OrderBookDeltas => Event::OrderBookDeltas(r.get()?),
            EventKind::Bar => Event::Bar(r.get()?),
            EventKind::MarkPriceUpdate => Event::MarkPriceUpdate(r.get()?),
            EventKind::IndexPriceUpdate => Event::IndexPriceUpdate(r.get()?),
            EventKind::FundingRateUpdate => Event::FundingRateUpdate(r.get()?),
            EventKind::InstrumentStatus => Event::InstrumentStatus(r.get()?),
            EventKind::InstrumentClose => Event::InstrumentClose(r.get()?),
            EventKind::LiquidationOrder => Event::LiquidationOrder(r.get()?),
            EventKind::Instrument => Event::Instrument(r.get()?),
            EventKind::Order => Event::Order(r.get()?),
            EventKind::AccountState => Event::AccountState(r.get()?),
            EventKind::RateLimitFeedback => Event::RateLimitFeedback(r.get()?),
            EventKind::TimerFired => Event::TimerFired(r.get()?),
            EventKind::BatchEnd => Event::BatchEnd(r.get()?),
            EventKind::Shutdown => Event::Shutdown(r.get()?),
        })
    }
}

/// The tag and the body: how an event stands alone outside a log record.
impl Wire for Event {
    fn encode(&self, w: &mut WireWriter) {
        w.u16(self.kind().tag());
        self.encode_body(w);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let kind = EventKind::from_tag(r.u16()?)?;
        Self::decode_body(kind, r)
    }
}
