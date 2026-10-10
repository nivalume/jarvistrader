//! What a strategy asks for (docs/architecture.md section 9.4). The kernel assigns the
//! `ClientOrderId`, runs both risk gates and turns the intent into an order and a command. It lives
//! in the execution layer so the risk layer can check intents.

use kernel_core::UnixNanos;
use model::enums::{OrderSide, OrderType, TimeInForce};
use model::{InstrumentId, Price, Quantity};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OrderIntent {
    pub instrument_id: InstrumentId,
    pub side: OrderSide,
    pub order_type: OrderType,
    pub quantity: Quantity,
    /// `Limit` only.
    pub price: Option<Price>,
    pub time_in_force: TimeInForce,
    /// Binance GTX.
    pub post_only: bool,
    /// One-way mode only.
    pub reduce_only: bool,
    /// `Gtd` only.
    pub expire_time: Option<UnixNanos>,
}

impl OrderIntent {
    #[must_use]
    pub const fn limit(
        instrument_id: InstrumentId,
        side: OrderSide,
        quantity: Quantity,
        price: Price,
        time_in_force: TimeInForce,
    ) -> Self {
        Self {
            instrument_id,
            side,
            order_type: OrderType::Limit,
            quantity,
            price: Some(price),
            time_in_force,
            post_only: false,
            reduce_only: false,
            expire_time: None,
        }
    }

    #[must_use]
    pub const fn market(instrument_id: InstrumentId, side: OrderSide, quantity: Quantity) -> Self {
        Self {
            instrument_id,
            side,
            order_type: OrderType::Market,
            quantity,
            price: None,
            time_in_force: TimeInForce::Ioc,
            post_only: false,
            reduce_only: false,
            expire_time: None,
        }
    }

    #[must_use]
    pub const fn post_only(mut self, post_only: bool) -> Self {
        self.post_only = post_only;
        self
    }
    #[must_use]
    pub const fn reduce_only(mut self, reduce_only: bool) -> Self {
        self.reduce_only = reduce_only;
        self
    }
    #[must_use]
    pub const fn expiring(mut self, at: UnixNanos) -> Self {
        self.time_in_force = TimeInForce::Gtd;
        self.expire_time = Some(at);
        self
    }
}

model::wire_struct!(OrderIntent {
    instrument_id,
    side,
    order_type,
    quantity,
    price,
    time_in_force,
    post_only,
    reduce_only,
    expire_time
});
