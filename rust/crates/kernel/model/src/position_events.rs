//! Position events (docs/architecture.md section 6.7). They are handed to strategies and are not
//! written to the event log (section 5.1); quantities and prices are fixed-point, and the signed
//! quantity is a raw `i64` on the 1e9 scale (section 6.8).

use kernel_core::{FixedString, Result, Status, UnixNanos};

use crate::currency::Currency;
use crate::enums::{OrderSide, PositionAdjustmentType, PositionSide};
use crate::fixed_point::{Price, Quantity};
use crate::identifiers::{
    AccountId, ClientOrderId, InstrumentId, PositionId, StrategyId, TraderId,
};
use crate::money::Money;
use crate::uuid::Uuid4;
use crate::wire_struct;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PositionEventHeader {
    pub trader_id: TraderId,
    pub strategy_id: StrategyId,
    pub instrument_id: InstrumentId,
    pub position_id: PositionId,
    pub account_id: AccountId,
    pub event_id: Uuid4,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}
wire_struct!(PositionEventHeader {
    trader_id,
    strategy_id,
    instrument_id,
    position_id,
    account_id,
    event_id,
    ts_event,
    ts_init
});

/// The position after a fill. `signed_qty` is positive long, negative short, on the 1e9 scale.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PositionState {
    pub opening_order_id: ClientOrderId,
    pub entry: OrderSide,
    pub side: PositionSide,
    pub signed_qty: i64,
    pub quantity: Quantity,
    pub peak_quantity: Quantity,
    pub last_qty: Quantity,
    pub last_px: Price,
    pub currency: Currency,
    pub avg_px_open: Price,
    pub avg_px_close: Option<Price>,
    /// Fixed-point on the 1e9 scale; `1.0` is `FIXED_SCALAR`.
    pub realized_return: i64,
    pub realized_pnl: Money,
    pub unrealized_pnl: Option<Money>,
}
wire_struct!(PositionState {
    opening_order_id,
    entry,
    side,
    signed_qty,
    quantity,
    peak_quantity,
    last_qty,
    last_px,
    currency,
    avg_px_open,
    avg_px_close,
    realized_return,
    realized_pnl,
    unrealized_pnl
});

impl PositionState {
    /// The side agrees with the sign, and the magnitude with the quantity.
    pub fn validate(&self) -> Result<()> {
        let expected = match self.side {
            PositionSide::Flat => 0,
            PositionSide::Long => {
                i64::try_from(self.quantity.raw()).map_err(|_| Status::Overflow)?
            }
            PositionSide::Short => {
                -i64::try_from(self.quantity.raw()).map_err(|_| Status::Overflow)?
            }
        };
        if self.signed_qty != expected || self.peak_quantity < self.quantity {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PositionOpened {
    pub header: PositionEventHeader,
    pub state: PositionState,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PositionChanged {
    pub header: PositionEventHeader,
    pub state: PositionState,
    pub ts_opened: UnixNanos,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PositionClosed {
    pub header: PositionEventHeader,
    pub state: PositionState,
    pub closing_order_id: ClientOrderId,
    pub ts_opened: UnixNanos,
    pub ts_closed: UnixNanos,
    pub duration: kernel_core::DurationNanos,
}
/// Funding, or commission paid in the base currency, applied to a position.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PositionAdjusted {
    pub header: PositionEventHeader,
    pub adjustment_type: PositionAdjustmentType,
    pub quantity_change: i64,
    pub pnl_change: Money,
    pub reason: Option<FixedString<96>>,
}
wire_struct!(PositionOpened { header, state });
wire_struct!(PositionChanged { header, state, ts_opened });
wire_struct!(PositionClosed { header, state, closing_order_id, ts_opened, ts_closed, duration });
wire_struct!(PositionAdjusted { header, adjustment_type, quantity_change, pnl_change, reason });

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PositionEvent {
    Opened(PositionOpened),
    Changed(PositionChanged),
    Closed(PositionClosed),
    Adjusted(PositionAdjusted),
}

impl PositionEvent {
    #[must_use]
    pub const fn header(&self) -> &PositionEventHeader {
        match self {
            PositionEvent::Opened(e) => &e.header,
            PositionEvent::Changed(e) => &e.header,
            PositionEvent::Closed(e) => &e.header,
            PositionEvent::Adjusted(e) => &e.header,
        }
    }
}
