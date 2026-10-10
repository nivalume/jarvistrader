//! Kernel outputs (docs/architecture.md sections 5.1, 9.1 and 16.1): what `step` produces. A
//! run log writes them after the input that caused them, so a replay recomputes them from the
//! inputs and compares byte for byte.
//!
//! Venue commands go to the node's order sender (live) or the simulated exchange (backtest); the
//! venue answers with order events. `OrderDenied` is a decision without a command, still an output
//! so that replay checks it. Reconciliation outputs and the node lifecycle arrive with their layers
//! (docs/rust-plan.md R4, R5).

use kernel_core::{FixedString, Result, Status, UnixNanos};

use crate::enums::{OrderSide, OrderType, TimeInForce};
use crate::fixed_point::{Price, Quantity};
use crate::identifiers::{ClientOrderId, InstrumentId, VenueOrderId};
use crate::order_events::OrderDenied;
use crate::wire::{Wire, WireReader, WireWriter};
use crate::wire_struct;

/// A kernel feature value delivered to at least one subscriber; `value` is on the 1e9 scale.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FeatureUpdate {
    pub feature_id: u32,
    pub value: i64,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}
wire_struct!(FeatureUpdate { feature_id, value, ts_event, ts_init });

pub type RecordTag = FixedString<32>;

/// A value a strategy recorded with `ctx.record(tag, value)`: its deterministic, replay-checked
/// output when it does not trade. `value` is on the 1e9 scale.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct StrategyRecord {
    pub strategy_index: u16,
    pub tag: RecordTag,
    pub value: i64,
    pub ts_init: UnixNanos,
}
wire_struct!(StrategyRecord { strategy_index, tag, value, ts_init });

/// A new order, after both risk gates passed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SubmitOrder {
    pub strategy_index: u16,
    pub client_order_id: ClientOrderId,
    pub instrument_id: InstrumentId,
    pub order_side: OrderSide,
    pub order_type: OrderType,
    pub quantity: Quantity,
    pub price: Option<Price>,
    pub time_in_force: TimeInForce,
    pub post_only: bool,
    pub reduce_only: bool,
    pub expire_time: Option<UnixNanos>,
    pub ts_init: UnixNanos,
}
wire_struct!(SubmitOrder {
    strategy_index,
    client_order_id,
    instrument_id,
    order_side,
    order_type,
    quantity,
    price,
    time_in_force,
    post_only,
    reduce_only,
    expire_time,
    ts_init
});

/// Binance `order.modify`: a LIMIT order's new quantity and price.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ModifyOrder {
    pub strategy_index: u16,
    pub client_order_id: ClientOrderId,
    pub instrument_id: InstrumentId,
    pub venue_order_id: Option<VenueOrderId>,
    pub quantity: Quantity,
    pub price: Price,
    pub ts_init: UnixNanos,
}
wire_struct!(ModifyOrder {
    strategy_index,
    client_order_id,
    instrument_id,
    venue_order_id,
    quantity,
    price,
    ts_init
});

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CancelOrder {
    pub strategy_index: u16,
    pub client_order_id: ClientOrderId,
    pub instrument_id: InstrumentId,
    pub venue_order_id: Option<VenueOrderId>,
    pub ts_init: UnixNanos,
}
wire_struct!(CancelOrder {
    strategy_index,
    client_order_id,
    instrument_id,
    venue_order_id,
    ts_init
});

/// Every open order of the instrument (the kill switch and `ctx.cancel_all`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CancelAllOrders {
    pub strategy_index: u16,
    pub instrument_id: InstrumentId,
    pub ts_init: UnixNanos,
}
wire_struct!(CancelAllOrders { strategy_index, instrument_id, ts_init });

/// The venue-side dead man's switch (section 10.3): the venue cancels every open order of the
/// instrument unless this is renewed within `countdown_ms`; 0 disarms it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CountdownCancelAll {
    pub instrument_id: InstrumentId,
    pub countdown_ms: u32,
    pub ts_init: UnixNanos,
}
wire_struct!(CountdownCancelAll { instrument_id, countdown_ms, ts_init });

/// The closed set of outputs. The discriminants are the wire tags.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Output {
    FeatureUpdate(FeatureUpdate),
    StrategyRecord(StrategyRecord),
    SubmitOrder(SubmitOrder),
    ModifyOrder(ModifyOrder),
    CancelOrder(CancelOrder),
    CancelAllOrders(CancelAllOrders),
    OrderDenied(OrderDenied),
    CountdownCancelAll(CountdownCancelAll),
}

macro_rules! output_dispatch {
    ($($variant:ident = $tag:literal),+ $(,)?) => {
        impl Output {
            #[must_use]
            pub const fn name(&self) -> &'static str {
                match self { $(Output::$variant(_) => stringify!($variant),)+ }
            }
            #[must_use]
            pub const fn tag(&self) -> u16 {
                match self { $(Output::$variant(_) => $tag,)+ }
            }
            /// Encodes the body (without the tag; the log record carries it).
            pub fn encode_body(&self, w: &mut WireWriter) {
                match self { $(Output::$variant(e) => w.put(e),)+ }
            }
            /// Decodes a body of the given tag.
            pub fn decode_body(tag: u16, r: &mut WireReader<'_>) -> Result<Self> {
                match tag {
                    $($tag => Ok(Output::$variant(r.get()?)),)+
                    _ => Err(Status::UnsupportedMessage),
                }
            }
        }
    };
}
output_dispatch!(
    FeatureUpdate = 1,
    StrategyRecord = 2,
    SubmitOrder = 3,
    ModifyOrder = 4,
    CancelOrder = 5,
    CancelAllOrders = 6,
    OrderDenied = 7,
    CountdownCancelAll = 8,
);

impl Output {
    /// The order a command concerns, if any.
    #[must_use]
    pub const fn client_order_id(&self) -> Option<&ClientOrderId> {
        match self {
            Output::SubmitOrder(c) => Some(&c.client_order_id),
            Output::ModifyOrder(c) => Some(&c.client_order_id),
            Output::CancelOrder(c) => Some(&c.client_order_id),
            Output::OrderDenied(d) => Some(&d.header.client_order_id),
            Output::FeatureUpdate(_)
            | Output::StrategyRecord(_)
            | Output::CancelAllOrders(_)
            | Output::CountdownCancelAll(_) => None,
        }
    }

    /// Whether a venue acts on this output.
    #[must_use]
    pub const fn is_venue_command(&self) -> bool {
        matches!(
            self,
            Output::SubmitOrder(_)
                | Output::ModifyOrder(_)
                | Output::CancelOrder(_)
                | Output::CancelAllOrders(_)
                | Output::CountdownCancelAll(_)
        )
    }
}

impl Wire for Output {
    fn encode(&self, w: &mut WireWriter) {
        w.u16(self.tag());
        self.encode_body(w);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let tag = r.u16()?;
        Self::decode_body(tag, r)
    }
}
