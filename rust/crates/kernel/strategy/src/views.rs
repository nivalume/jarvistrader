//! Copies of kernel state a strategy reads through its [`crate::Context`].

use kernel_core::UnixNanos;
use model::enums::{OrderSide, OrderStatus, OrderType, PositionSide, TimeInForce};
use model::{ClientOrderId, InstrumentId, Money, PositionId, Price, Quantity, VenueOrderId};

/// One order (`ctx.order`, `ctx.open_orders`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OrderView {
    pub client_order_id: ClientOrderId,
    pub venue_order_id: Option<VenueOrderId>,
    pub instrument_id: InstrumentId,
    pub side: OrderSide,
    pub order_type: OrderType,
    pub time_in_force: TimeInForce,
    pub post_only: bool,
    pub reduce_only: bool,
    pub price: Option<Price>,
    pub status: OrderStatus,
    pub quantity: Quantity,
    pub filled: Quantity,
    pub leaves: Quantity,
    /// Truncated to the instrument's price precision.
    pub avg_px: Option<Price>,
    pub ts_init: UnixNanos,
}

/// A strategy's position in one instrument (its share of the venue position, from its own
/// fills). PnL amounts are in the settlement currency; `realized_pnl` is net of commissions and
/// funding since the position opened.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PositionView {
    pub instrument_id: InstrumentId,
    pub position_id: PositionId,
    pub side: PositionSide,
    /// Raw at the 1e9 scale, positive long.
    pub signed_raw: i64,
    pub quantity: Quantity,
    /// Full precision.
    pub avg_px_open: Option<Price>,
    pub realized_pnl: Money,
    /// At the mark price, else the last trade.
    pub unrealized_pnl: Money,
    pub commission: Money,
    pub funding: Money,
    /// Realized, net of commissions and funding, since the node started.
    pub total_pnl: Money,
    pub ts_opened: UnixNanos,
}

/// `open_exposure()` of one instrument (docs/architecture.md section 9.3): the venue position
/// (all strategies) plus the leaves of open orders.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ExposureView {
    pub instrument_id: InstrumentId,
    /// Signed, raw at the 1e9 scale.
    pub position_raw: i64,
    /// Leaves of open buy orders.
    pub open_buy: Quantity,
    pub open_sell: Quantity,
    /// `position + open buys`, raw.
    pub max_long_raw: i64,
    /// `position - open sells`, raw.
    pub max_short_raw: i64,
    /// The larger of `|max_long|` and `|max_short|` at the valuation price.
    pub notional: Option<Money>,
}
