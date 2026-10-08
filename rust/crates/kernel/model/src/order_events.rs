//! The 17 order events (docs/architecture.md section 6.7). Every event starts with the same
//! [`OrderEventHeader`]; the free dictionaries and lists nautilus carries (`linked_order_ids`,
//! `exec_algorithm_params`, `tags`, `info`) are not in the kernel model (section 6.8), and a
//! fill's venue flags travel as [`FillInfoFlags`].

use kernel_core::{FixedString, Result, Status, UnixNanos};

use crate::currency::Currency;
use crate::enums::{
    ContingencyType, LiquiditySide, OrderSide, OrderType, TimeInForce, TrailingOffsetType,
    TriggerType,
};
use crate::fixed_point::{Price, Quantity};
use crate::identifiers::{
    AccountId, ClientOrderId, ExecAlgorithmId, InstrumentId, OrderListId, PositionId, StrategyId,
    TradeId, TraderId, VenueOrderId,
};
use crate::money::Money;
use crate::uuid::Uuid4;
use crate::wire::{Wire, WireReader, WireWriter};
use crate::wire_struct;

/// A `CATEGORY_CONDITION` reason code or a venue's message, for example
/// `NOTIONAL_EXCEEDS_MAX_PER_ORDER`.
pub type Reason = FixedString<96>;

/// What every order event carries.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OrderEventHeader {
    pub trader_id: TraderId,
    pub strategy_id: StrategyId,
    pub instrument_id: InstrumentId,
    pub client_order_id: ClientOrderId,
    pub event_id: Uuid4,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
    /// The event that caused this one, when known.
    pub causation_id: Option<Uuid4>,
}
wire_struct!(OrderEventHeader {
    trader_id,
    strategy_id,
    instrument_id,
    client_order_id,
    event_id,
    ts_event,
    ts_init,
    causation_id
});

impl OrderEventHeader {
    pub fn validate(&self) -> Result<()> {
        if self.ts_init < self.ts_event {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}

/// Venue flags on a fill, in place of nautilus's `info` dictionary.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct FillInfoFlags(pub u8);
impl FillInfoFlags {
    pub const LIQUIDATION: u8 = 1;
    pub const AUTO_DELEVERAGING: u8 = 2;
    pub const TRADE_LITE: u8 = 4;
    pub const MASK: u8 = 7;

    pub const fn new(bits: u8) -> Result<Self> {
        if bits & !Self::MASK != 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(Self(bits))
    }
    #[must_use]
    pub const fn is_set(self, flag: u8) -> bool {
        self.0 & flag != 0
    }
}
impl Wire for FillInfoFlags {
    fn encode(&self, w: &mut WireWriter) {
        w.u8(self.0);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.u8()?)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[allow(clippy::struct_excessive_bools)] // nautilus's field set; each flag is independent
pub struct OrderInitialized {
    pub header: OrderEventHeader,
    pub order_side: OrderSide,
    pub order_type: OrderType,
    pub quantity: Quantity,
    pub time_in_force: TimeInForce,
    pub post_only: bool,
    pub reduce_only: bool,
    pub quote_quantity: bool,
    pub reconciliation: bool,
    pub price: Option<Price>,
    pub activation_price: Option<Price>,
    pub trigger_price: Option<Price>,
    pub trigger_type: Option<TriggerType>,
    pub limit_offset: Option<Price>,
    pub trailing_offset: Option<Price>,
    pub trailing_offset_type: Option<TrailingOffsetType>,
    pub expire_time: Option<UnixNanos>,
    pub display_qty: Option<Quantity>,
    pub emulation_trigger: Option<TriggerType>,
    pub trigger_instrument_id: Option<InstrumentId>,
    pub contingency_type: Option<ContingencyType>,
    pub order_list_id: Option<OrderListId>,
    pub parent_order_id: Option<ClientOrderId>,
    pub exec_algorithm_id: Option<ExecAlgorithmId>,
    pub exec_spawn_id: Option<ClientOrderId>,
}

impl OrderInitialized {
    /// A positive quantity; a limit-type order has a price and a market-type order has none; a
    /// stop-type order has a trigger price; GTD has an expiry.
    pub fn validate(&self) -> Result<()> {
        self.header.validate()?;
        if self.quantity.is_undef() || self.quantity.is_zero() {
            return Err(Status::InvalidArgument);
        }
        let needs_price = matches!(
            self.order_type,
            OrderType::Limit
                | OrderType::StopLimit
                | OrderType::LimitIfTouched
                | OrderType::TrailingStopLimit
        );
        if needs_price != self.price.is_some() {
            return Err(Status::InvalidArgument);
        }
        let needs_trigger = matches!(
            self.order_type,
            OrderType::StopMarket
                | OrderType::StopLimit
                | OrderType::MarketIfTouched
                | OrderType::LimitIfTouched
        );
        if needs_trigger && self.trigger_price.is_none() {
            return Err(Status::InvalidArgument);
        }
        if (self.time_in_force == TimeInForce::Gtd) != self.expire_time.is_some() {
            return Err(Status::InvalidArgument);
        }
        if self.price.is_some_and(Price::is_undef)
            || self.trigger_price.is_some_and(Price::is_undef)
        {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}

macro_rules! validated_wire {
    ($t:ident { $($f:ident),+ $(,)? }) => {
        impl Wire for $t {
            fn encode(&self, w: &mut WireWriter) {
                $( w.put(&self.$f); )+
            }
            fn decode(r: &mut WireReader<'_>) -> Result<Self> {
                let v = $t { $( $f: r.get()?, )+ };
                v.validate()?;
                Ok(v)
            }
        }
    };
}
validated_wire!(OrderInitialized {
    header,
    order_side,
    order_type,
    quantity,
    time_in_force,
    post_only,
    reduce_only,
    quote_quantity,
    reconciliation,
    price,
    activation_price,
    trigger_price,
    trigger_type,
    limit_offset,
    trailing_offset,
    trailing_offset_type,
    expire_time,
    display_qty,
    emulation_trigger,
    trigger_instrument_id,
    contingency_type,
    order_list_id,
    parent_order_id,
    exec_algorithm_id,
    exec_spawn_id
});

/// Declares an event that is the header plus a few fields, validated by the header alone.
macro_rules! simple_order_event {
    ($(#[$doc:meta])* $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
        $(#[$doc])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub struct $name {
            pub header: OrderEventHeader,
            $(pub $f: $t,)*
        }
        impl $name {
            pub fn validate(&self) -> Result<()> {
                self.header.validate()
            }
        }
        validated_wire!($name { header $(, $f)* });
    };
}

simple_order_event!(OrderDenied { reason: Reason });
simple_order_event!(OrderEmulated {});
simple_order_event!(OrderReleased { released_price: Price });
simple_order_event!(OrderSubmitted { account_id: AccountId });
simple_order_event!(OrderAccepted {
    venue_order_id: VenueOrderId,
    account_id: AccountId,
    reconciliation: bool
});
simple_order_event!(
    /// `due_post_only`: a post-only order rejected because it would have taken liquidity.
    OrderRejected { account_id: AccountId, reason: Reason, reconciliation: bool, due_post_only: bool }
);
simple_order_event!(OrderCanceled { venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reason: Option<Reason>, reconciliation: bool });
simple_order_event!(OrderExpired { venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reconciliation: bool });
simple_order_event!(OrderTriggered { venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reconciliation: bool });
simple_order_event!(OrderPendingUpdate { account_id: AccountId, venue_order_id: Option<VenueOrderId>, reconciliation: bool });
simple_order_event!(OrderPendingCancel { account_id: AccountId, venue_order_id: Option<VenueOrderId>, reconciliation: bool });
simple_order_event!(OrderModifyRejected { reason: Reason, venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reconciliation: bool });
simple_order_event!(OrderCancelRejected { reason: Reason, venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reconciliation: bool });

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OrderUpdated {
    pub header: OrderEventHeader,
    pub venue_order_id: Option<VenueOrderId>,
    pub account_id: Option<AccountId>,
    pub quantity: Quantity,
    pub price: Option<Price>,
    pub trigger_price: Option<Price>,
    pub protection_price: Option<Price>,
    pub is_quote_quantity: bool,
    pub reconciliation: bool,
}
impl OrderUpdated {
    pub fn validate(&self) -> Result<()> {
        self.header.validate()?;
        if self.quantity.is_undef() || self.quantity.is_zero() {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}
validated_wire!(OrderUpdated {
    header,
    venue_order_id,
    account_id,
    quantity,
    price,
    trigger_price,
    protection_price,
    is_quote_quantity,
    reconciliation
});

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OrderFilled {
    pub header: OrderEventHeader,
    pub venue_order_id: VenueOrderId,
    pub account_id: AccountId,
    pub trade_id: TradeId,
    pub order_side: OrderSide,
    pub order_type: OrderType,
    pub last_qty: Quantity,
    pub last_px: Price,
    pub currency: Currency,
    pub liquidity_side: LiquiditySide,
    pub reconciliation: bool,
    pub position_id: Option<PositionId>,
    pub commission: Option<Money>,
    pub info_flags: FillInfoFlags,
}
impl OrderFilled {
    /// A positive fill at a real price; a commission, if any, in some currency (it need not be
    /// `currency`, which is the quote currency of the fill).
    pub fn validate(&self) -> Result<()> {
        self.header.validate()?;
        if self.last_qty.is_undef() || self.last_qty.is_zero() || self.last_px.is_undef() {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}
validated_wire!(OrderFilled {
    header,
    venue_order_id,
    account_id,
    trade_id,
    order_side,
    order_type,
    last_qty,
    last_px,
    currency,
    liquidity_side,
    reconciliation,
    position_id,
    commission,
    info_flags
});

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OrderFillVoided {
    pub header: OrderEventHeader,
    pub venue_order_id: VenueOrderId,
    pub account_id: AccountId,
    pub correction_id: TradeId,
    pub trade_id: TradeId,
    pub voided_qty: Quantity,
    pub commission_voided: Option<Money>,
    pub order_side: OrderSide,
    pub order_type: OrderType,
    pub last_px: Price,
    pub currency: Currency,
    pub liquidity_side: LiquiditySide,
    pub position_id: Option<PositionId>,
    pub reason: Option<Reason>,
    pub reconciliation: bool,
    pub is_reopened: bool,
    pub info_flags: FillInfoFlags,
}
impl OrderFillVoided {
    pub fn validate(&self) -> Result<()> {
        self.header.validate()?;
        if self.voided_qty.is_undef() || self.voided_qty.is_zero() || self.last_px.is_undef() {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}
validated_wire!(OrderFillVoided {
    header,
    venue_order_id,
    account_id,
    correction_id,
    trade_id,
    voided_qty,
    commission_voided,
    order_side,
    order_type,
    last_px,
    currency,
    liquidity_side,
    position_id,
    reason,
    reconciliation,
    is_reopened,
    info_flags
});

/// The closed set of order events. The discriminants are the wire tags.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum OrderEvent {
    Initialized(OrderInitialized),
    Denied(OrderDenied),
    Emulated(OrderEmulated),
    Released(OrderReleased),
    Submitted(OrderSubmitted),
    Accepted(OrderAccepted),
    Rejected(OrderRejected),
    Canceled(OrderCanceled),
    Expired(OrderExpired),
    Triggered(OrderTriggered),
    PendingUpdate(OrderPendingUpdate),
    PendingCancel(OrderPendingCancel),
    ModifyRejected(OrderModifyRejected),
    CancelRejected(OrderCancelRejected),
    Updated(OrderUpdated),
    Filled(OrderFilled),
    FillVoided(OrderFillVoided),
}

macro_rules! order_event_dispatch {
    ($($variant:ident = $tag:literal),+ $(,)?) => {
        impl OrderEvent {
            /// The nautilus event name, for example `OrderFilled`.
            #[must_use]
            pub const fn name(&self) -> &'static str {
                match self { $(OrderEvent::$variant(_) => concat!("Order", stringify!($variant)),)+ }
            }
            /// The wire tag of this variant.
            #[must_use]
            pub const fn tag(&self) -> u8 {
                match self { $(OrderEvent::$variant(_) => $tag,)+ }
            }
            #[must_use]
            pub const fn header(&self) -> &OrderEventHeader {
                match self { $(OrderEvent::$variant(e) => &e.header,)+ }
            }
            pub fn validate(&self) -> Result<()> {
                match self { $(OrderEvent::$variant(e) => e.validate(),)+ }
            }
        }
        impl Wire for OrderEvent {
            fn encode(&self, w: &mut WireWriter) {
                w.u8(self.tag());
                match self { $(OrderEvent::$variant(e) => w.put(e),)+ }
            }
            fn decode(r: &mut WireReader<'_>) -> Result<Self> {
                match r.u8()? {
                    $($tag => Ok(OrderEvent::$variant(r.get()?)),)+
                    _ => Err(Status::InvalidArgument),
                }
            }
        }
    };
}
order_event_dispatch!(
    Initialized = 1,
    Denied = 2,
    Emulated = 3,
    Released = 4,
    Submitted = 5,
    Accepted = 6,
    Rejected = 7,
    Canceled = 8,
    Expired = 9,
    Triggered = 10,
    PendingUpdate = 11,
    PendingCancel = 12,
    ModifyRejected = 13,
    CancelRejected = 14,
    Updated = 15,
    Filled = 16,
    FillVoided = 17,
);
