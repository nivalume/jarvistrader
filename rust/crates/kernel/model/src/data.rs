//! Market data types (docs/architecture.md section 6.4). Each has a validating constructor; the
//! wire decoders go through it.

use kernel_core::{FixedVec, Result, Status, UnixNanos};

use crate::enums::{
    AggressorSide, BookAction, InstrumentCloseType, MarketStatusAction, OrderSide, RecordFlag,
};
use crate::fixed_point::{Price, Quantity};
use crate::identifiers::{InstrumentId, TradeId};
use crate::wire::{Wire, WireReader, WireWriter};
use crate::wire_struct;

fn check_ts(ts_event: UnixNanos, ts_init: UnixNanos) -> Result<()> {
    if ts_init < ts_event {
        return Err(Status::InvalidArgument);
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TradeTick {
    pub instrument_id: InstrumentId,
    pub price: Price,
    pub size: Quantity,
    pub aggressor_side: AggressorSide,
    pub trade_id: TradeId,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl TradeTick {
    /// A real price, a positive size, `ts_init >= ts_event`.
    pub fn new(
        instrument_id: InstrumentId,
        price: Price,
        size: Quantity,
        aggressor_side: AggressorSide,
        trade_id: TradeId,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if price.is_undef() || size.is_undef() || size.is_zero() {
            return Err(Status::InvalidArgument);
        }
        check_ts(ts_event, ts_init)?;
        Ok(Self { instrument_id, price, size, aggressor_side, trade_id, ts_event, ts_init })
    }
}
impl Wire for TradeTick {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.price);
        w.put(&self.size);
        w.put(&self.aggressor_side);
        w.put(&self.trade_id);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct QuoteTick {
    pub instrument_id: InstrumentId,
    pub bid_price: Price,
    pub ask_price: Price,
    pub bid_size: Quantity,
    pub ask_size: Quantity,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl QuoteTick {
    /// Real prices and sizes, `ts_init >= ts_event`. A crossed or empty book is allowed: venues
    /// report both, and the strategy decides.
    pub fn new(
        instrument_id: InstrumentId,
        bid_price: Price,
        ask_price: Price,
        bid_size: Quantity,
        ask_size: Quantity,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if bid_price.is_undef()
            || ask_price.is_undef()
            || bid_size.is_undef()
            || ask_size.is_undef()
        {
            return Err(Status::InvalidArgument);
        }
        check_ts(ts_event, ts_init)?;
        Ok(Self { instrument_id, bid_price, ask_price, bid_size, ask_size, ts_event, ts_init })
    }

    /// `(bid + ask) / 2` at the larger precision plus one digit, as nautilus's `extract_price(MID)`;
    /// `Overflow` only at the edge of the range.
    pub fn mid_price(&self) -> Result<Price> {
        let sum = i128::from(self.bid_price.raw()) + i128::from(self.ask_price.raw());
        let precision = self
            .bid_price
            .precision()
            .max(self.ask_price.precision())
            .min(crate::fixed_point::FIXED_PRECISION - 1)
            + 1;
        let step = i128::from(
            kernel_core::int_math::POW10
                [(crate::fixed_point::FIXED_PRECISION - precision) as usize],
        );
        let half = sum / 2;
        let raw = half - half % step; // toward zero to the precision
        Price::from_raw(i64::try_from(raw).map_err(|_| Status::Overflow)?, precision)
    }
}
impl Wire for QuoteTick {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.bid_price);
        w.put(&self.ask_price);
        w.put(&self.bid_size);
        w.put(&self.ask_size);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?)
    }
}

/// One order or level in a book; `side` is `None` for a `Clear`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct BookOrder {
    pub side: Option<OrderSide>,
    pub price: Price,
    pub size: Quantity,
    pub order_id: u64,
}
wire_struct!(BookOrder { side, price, size, order_id });

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OrderBookDelta {
    pub instrument_id: InstrumentId,
    pub action: BookAction,
    pub order: BookOrder,
    pub flags: u8,
    pub sequence: u64,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl OrderBookDelta {
    /// A `Clear` carries no side and a zero order; the other actions carry a side and a real
    /// price. Flags must be a combination of [`RecordFlag`] bits.
    pub fn new(
        instrument_id: InstrumentId,
        action: BookAction,
        order: BookOrder,
        flags: u8,
        sequence: u64,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if flags & !RECORD_FLAG_MASK != 0 {
            return Err(Status::InvalidArgument);
        }
        match action {
            BookAction::Clear => {
                if order.side.is_some() || order.order_id != 0 {
                    return Err(Status::InvalidArgument);
                }
            }
            _ => {
                if order.side.is_none() || order.price.is_undef() || order.size.is_undef() {
                    return Err(Status::InvalidArgument);
                }
            }
        }
        check_ts(ts_event, ts_init)?;
        Ok(Self { instrument_id, action, order, flags, sequence, ts_event, ts_init })
    }

    /// A `Clear` at the start of a snapshot.
    pub fn clear(
        instrument_id: InstrumentId,
        sequence: u64,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        Self::new(
            instrument_id,
            BookAction::Clear,
            BookOrder::default(),
            0,
            sequence,
            ts_event,
            ts_init,
        )
    }
}
impl Wire for OrderBookDelta {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.action);
        w.put(&self.order);
        w.u8(self.flags);
        w.u64(self.sequence);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.u8()?, r.u64()?, r.get()?, r.get()?)
    }
}

pub const RECORD_FLAG_MASK: u8 = RecordFlag::Last as u8
    | RecordFlag::Tob as u8
    | RecordFlag::Snapshot as u8
    | RecordFlag::Mbp as u8
    | RecordFlag::Reserved2 as u8
    | RecordFlag::Reserved1 as u8;

/// A batch of deltas for one instrument. `flags` and `sequence` are the last delta's; a snapshot
/// starts with a `Clear` and its last delta carries `F_SNAPSHOT | F_LAST`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OrderBookDeltas {
    pub instrument_id: InstrumentId,
    pub deltas: FixedVec<OrderBookDelta>,
    pub flags: u8,
    pub sequence: u64,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl OrderBookDeltas {
    /// At least one delta, all for `instrument_id`; the batch's timestamps, flags and sequence
    /// come from the last delta.
    pub fn new(instrument_id: InstrumentId, deltas: FixedVec<OrderBookDelta>) -> Result<Self> {
        let last = deltas.last().ok_or(Status::InvalidArgument)?;
        if deltas.iter().any(|d| d.instrument_id != instrument_id) {
            return Err(Status::InvalidArgument);
        }
        let (flags, sequence, ts_event, ts_init) =
            (last.flags, last.sequence, last.ts_event, last.ts_init);
        Ok(Self { instrument_id, deltas, flags, sequence, ts_event, ts_init })
    }

    #[must_use]
    pub fn is_snapshot(&self) -> bool {
        self.deltas.first().is_some_and(|d| d.action == BookAction::Clear)
            && RecordFlag::Snapshot.is_set(self.flags)
    }
}
impl Wire for OrderBookDeltas {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.deltas);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?)
    }
}

/// Venue status change.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct InstrumentStatus {
    pub instrument_id: InstrumentId,
    pub action: MarketStatusAction,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
    pub reason: Option<kernel_core::FixedString<64>>,
    pub trading_event: Option<kernel_core::FixedString<64>>,
    pub is_trading: Option<bool>,
    pub is_quoting: Option<bool>,
    pub is_short_sell_restricted: Option<bool>,
}
wire_struct!(InstrumentStatus {
    instrument_id,
    action,
    ts_event,
    ts_init,
    reason,
    trading_event,
    is_trading,
    is_quoting,
    is_short_sell_restricted
});

macro_rules! single_value_update {
    ($(#[$doc:meta])* $name:ident, $field:ident) => {
        $(#[$doc])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub struct $name {
            pub instrument_id: InstrumentId,
            pub $field: Price,
            pub ts_event: UnixNanos,
            pub ts_init: UnixNanos,
        }
        impl $name {
            pub fn new(instrument_id: InstrumentId, $field: Price, ts_event: UnixNanos, ts_init: UnixNanos) -> Result<Self> {
                if $field.is_undef() {
                    return Err(Status::InvalidArgument);
                }
                check_ts(ts_event, ts_init)?;
                Ok(Self { instrument_id, $field, ts_event, ts_init })
            }
        }
        impl Wire for $name {
            fn encode(&self, w: &mut WireWriter) {
                w.put(&self.instrument_id);
                w.put(&self.$field);
                w.put(&self.ts_event);
                w.put(&self.ts_init);
            }
            fn decode(r: &mut WireReader<'_>) -> Result<Self> {
                Self::new(r.get()?, r.get()?, r.get()?, r.get()?)
            }
        }
    };
}
single_value_update!(MarkPriceUpdate, value);
single_value_update!(IndexPriceUpdate, value);

/// The funding rate as a fixed-point value on the 1e9 scale (nautilus keeps a decimal; the text
/// forms agree).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FundingRateUpdate {
    pub instrument_id: InstrumentId,
    pub rate: Price,
    /// Minutes between fundings.
    pub interval: Option<u32>,
    pub next_funding_ns: Option<UnixNanos>,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}
impl FundingRateUpdate {
    pub fn new(
        instrument_id: InstrumentId,
        rate: Price,
        interval: Option<u32>,
        next_funding_ns: Option<UnixNanos>,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if rate.is_undef() || interval == Some(0) {
            return Err(Status::InvalidArgument);
        }
        check_ts(ts_event, ts_init)?;
        Ok(Self { instrument_id, rate, interval, next_funding_ns, ts_event, ts_init })
    }
}
impl Wire for FundingRateUpdate {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.rate);
        w.put(&self.interval);
        w.put(&self.next_funding_ns);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct InstrumentClose {
    pub instrument_id: InstrumentId,
    pub close_price: Price,
    pub close_type: InstrumentCloseType,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}
impl InstrumentClose {
    pub fn new(
        instrument_id: InstrumentId,
        close_price: Price,
        close_type: InstrumentCloseType,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if close_price.is_undef() {
            return Err(Status::InvalidArgument);
        }
        check_ts(ts_event, ts_init)?;
        Ok(Self { instrument_id, close_price, close_type, ts_event, ts_init })
    }
}
impl Wire for InstrumentClose {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.close_price);
        w.put(&self.close_type);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.get()?, r.get()?)
    }
}

/// jarvis's extension type for Binance's `forceOrder` stream: a liquidation order.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct LiquidationOrder {
    pub instrument_id: InstrumentId,
    pub side: OrderSide,
    pub price: Price,
    pub average_price: Price,
    pub original_quantity: Quantity,
    pub filled_quantity: Quantity,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}
impl LiquidationOrder {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        instrument_id: InstrumentId,
        side: OrderSide,
        price: Price,
        average_price: Price,
        original_quantity: Quantity,
        filled_quantity: Quantity,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if price.is_undef()
            || average_price.is_undef()
            || original_quantity.is_undef()
            || filled_quantity.is_undef()
        {
            return Err(Status::InvalidArgument);
        }
        if filled_quantity > original_quantity {
            return Err(Status::InvalidArgument);
        }
        check_ts(ts_event, ts_init)?;
        Ok(Self {
            instrument_id,
            side,
            price,
            average_price,
            original_quantity,
            filled_quantity,
            ts_event,
            ts_init,
        })
    }
}
impl Wire for LiquidationOrder {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.instrument_id);
        w.put(&self.side);
        w.put(&self.price);
        w.put(&self.average_price);
        w.put(&self.original_quantity);
        w.put(&self.filled_quantity);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?)
    }
}
