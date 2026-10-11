//! Event classes: each kernel event type has a Python class whose attributes are the struct's
//! fields (order and position events flatten their headers, as nautilus does). The classes
//! themselves are made in Python (`model_py.py`) from the field tables here; this module converts
//! between the Python objects and the Rust structs and validates on construction.

use kernel_core::{DurationNanos, FixedString, FixedVec, Result as KResult, UnixNanos};
use model::account::{AccountBalance, AccountState, MarginBalance};
use model::bar::{Bar, BarType};
use model::data::*;
use model::enums::*;
use model::event::{
    BatchEnd, Event, NodeLifecycle, RateLimitFeedback, Shutdown, StrategyError, TimerFired,
};
use model::identifiers::*;
use model::instruments::{Instrument, InstrumentKind, InstrumentLimits};
use model::order_events::*;
use model::outputs::*;
use model::position_events::*;
use model::{Currency, Money, Price, Quantity, Uuid4};
use pyo3::exceptions::PyTypeError;
use pyo3::prelude::*;
use pyo3::types::{PyDict, PyList, PyString};

use crate::common::{check, type_error, validated, Dec9, DecPrice, DecQuantity, PyField};

/// One attribute of an event class.
pub(crate) struct FieldSpec {
    pub name: &'static str,
    /// `Some` when the argument may be left out (`None` for optionals, `False` for flags).
    pub default: fn(Python<'_>) -> Option<Py<PyAny>>,
}

/// A Rust struct with a Python event class.
pub(crate) trait PyStruct: Sized {
    const NAME: &'static str;
    const DOC: &'static str;
    fn fields() -> Vec<FieldSpec>;
    /// Sets the object's attributes from the struct.
    fn write(&self, obj: &Bound<'_, PyAny>) -> PyResult<()>;
    /// Reads and validates a struct from the object's attributes.
    fn read(obj: &Bound<'_, PyAny>) -> PyResult<Self>;
}

/// A new instance of the Python class `name` with the struct's attributes.
pub(crate) fn make<'py, T: PyStruct>(py: Python<'py>, value: &T) -> PyResult<Bound<'py, PyAny>> {
    let obj = crate::model::new_instance(py, T::NAME)?;
    value.write(&obj)?;
    Ok(obj)
}

impl<T: PyStruct + Clone> PyField for T {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        make(py, self)
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if crate::model::class_name(obj)? != T::NAME {
            return Err(type_error(what, &format!("a {}", T::NAME), obj));
        }
        T::read(obj)
    }
}

macro_rules! py_struct {
    ($t:ty, $name:expr, $doc:expr, { $($field:tt)* }) => {
        impl PyStruct for $t {
            const NAME: &'static str = $name;
            const DOC: &'static str = $doc;
            #[allow(clippy::vec_init_then_push)] // the macro appends one entry per field
            fn fields() -> Vec<FieldSpec> {
                let mut out = Vec::new();
                py_struct!(@fields out; $($field)*);
                out
            }
            fn write(&self, obj: &Bound<'_, PyAny>) -> PyResult<()> {
                py_struct!(@write self obj; $($field)*);
                Ok(())
            }
            fn read(obj: &Bound<'_, PyAny>) -> PyResult<Self> {
                let value = <$t>::from_parts(py_struct!(@read obj; $($field)*));
                validated(&value, $name)
            }
        }
    };
    // fields
    (@fields $out:ident;) => {};
    (@fields $out:ident; ($f:ident : $ty:ty) $($rest:tt)*) => {
        $out.push(FieldSpec { name: stringify!($f), default: <$ty as PyField>::default_py });
        py_struct!(@fields $out; $($rest)*);
    };
    (@fields $out:ident; ($f:ident : $ty:ty as $w:ty) $($rest:tt)*) => {
        $out.push(FieldSpec { name: stringify!($f), default: <$w as PyField>::default_py });
        py_struct!(@fields $out; $($rest)*);
    };
    (@fields $out:ident; (flat $f:ident : $ty:ty) $($rest:tt)*) => {
        $out.extend(<$ty as PyStruct>::fields());
        py_struct!(@fields $out; $($rest)*);
    };
    // write
    (@write $self:ident $obj:ident;) => {};
    (@write $self:ident $obj:ident; ($f:ident : $ty:ty) $($rest:tt)*) => {
        $obj.setattr(stringify!($f), PyField::to_py(&$self.$f, $obj.py())?)?;
        py_struct!(@write $self $obj; $($rest)*);
    };
    (@write $self:ident $obj:ident; ($f:ident : $ty:ty as $w:ty) $($rest:tt)*) => {
        $obj.setattr(stringify!($f), PyField::to_py(&<$w>::from($self.$f), $obj.py())?)?;
        py_struct!(@write $self $obj; $($rest)*);
    };
    (@write $self:ident $obj:ident; (flat $f:ident : $ty:ty) $($rest:tt)*) => {
        PyStruct::write(&$self.$f, $obj)?;
        py_struct!(@write $self $obj; $($rest)*);
    };
    // read: a tuple of the parts, in field order
    (@read $obj:ident; $($field:tt)*) => {
        ( $( py_struct!(@read1 $obj; $field), )* )
    };
    (@read1 $obj:ident; ($f:ident : $ty:ty)) => {
        <$ty as PyField>::from_py(&$obj.getattr(stringify!($f))?, stringify!($f))?
    };
    (@read1 $obj:ident; ($f:ident : $ty:ty as $w:ty)) => {
        <$w as PyField>::from_py(&$obj.getattr(stringify!($f))?, stringify!($f))?.into()
    };
    (@read1 $obj:ident; (flat $f:ident : $ty:ty)) => {
        <$ty as PyStruct>::read($obj)?
    };
}

/// Builds a struct from the tuple of its fields in declaration order (what `py_struct!` reads).
pub(crate) trait FromParts<P> {
    fn from_parts(parts: P) -> Self;
}

macro_rules! from_parts {
    ($t:ty { $($f:ident : $ty:ty),+ $(,)? }) => {
        impl FromParts<( $($ty,)+ )> for $t {
            fn from_parts(( $($f,)+ ): ( $($ty,)+ )) -> Self {
                Self { $($f,)+ }
            }
        }
    };
}

impl From<Dec9> for i64 {
    fn from(d: Dec9) -> Self {
        d.0
    }
}
impl From<i64> for Dec9 {
    fn from(v: i64) -> Self {
        Dec9(v)
    }
}
impl From<DecPrice> for Price {
    fn from(d: DecPrice) -> Self {
        d.0
    }
}
impl From<Price> for DecPrice {
    fn from(v: Price) -> Self {
        DecPrice(v)
    }
}
impl From<DecQuantity> for Quantity {
    fn from(d: DecQuantity) -> Self {
        d.0
    }
}
impl From<Quantity> for DecQuantity {
    fn from(v: Quantity) -> Self {
        DecQuantity(v)
    }
}

// ---- market data --------------------------------------------------------------------------

from_parts!(QuoteTick {
    instrument_id: InstrumentId,
    bid_price: Price,
    ask_price: Price,
    bid_size: Quantity,
    ask_size: Quantity,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(QuoteTick, "QuoteTick", "Best bid and ask.", {
    (instrument_id: InstrumentId) (bid_price: Price) (ask_price: Price) (bid_size: Quantity)
    (ask_size: Quantity) (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(TradeTick {
    instrument_id: InstrumentId,
    price: Price,
    size: Quantity,
    aggressor_side: AggressorSide,
    trade_id: TradeId,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(TradeTick, "TradeTick", "One trade (Binance aggTrade).", {
    (instrument_id: InstrumentId) (price: Price) (size: Quantity) (aggressor_side: AggressorSide)
    (trade_id: TradeId) (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(Bar {
    bar_type: BarType,
    open: Price,
    high: Price,
    low: Price,
    close: Price,
    volume: Quantity,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(Bar, "Bar", "OHLCV bar.", {
    (bar_type: BarType) (open: Price) (high: Price) (low: Price) (close: Price) (volume: Quantity)
    (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(OrderBookDelta {
    instrument_id: InstrumentId,
    action: BookAction,
    order: BookOrder,
    flags: u8,
    sequence: u64,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(OrderBookDelta, "OrderBookDelta", "One order book change.", {
    (instrument_id: InstrumentId) (action: BookAction) (order: BookOrder) (flags: u8) (sequence: u64)
    (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(InstrumentStatus { instrument_id: InstrumentId, action: MarketStatusAction, ts_event: UnixNanos, ts_init: UnixNanos, reason: Option<FixedString<64>>, trading_event: Option<FixedString<64>>, is_trading: Option<bool>, is_quoting: Option<bool>, is_short_sell_restricted: Option<bool> });
py_struct!(InstrumentStatus, "InstrumentStatus", "Trading status of an instrument.", {
    (instrument_id: InstrumentId) (action: MarketStatusAction) (ts_event: UnixNanos) (ts_init: UnixNanos)
    (reason: Option<FixedString<64>>) (trading_event: Option<FixedString<64>>) (is_trading: Option<bool>)
    (is_quoting: Option<bool>) (is_short_sell_restricted: Option<bool>)
});
from_parts!(MarkPriceUpdate {
    instrument_id: InstrumentId,
    value: Price,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(MarkPriceUpdate, "MarkPriceUpdate", "Mark price.", {
    (instrument_id: InstrumentId) (value: Price) (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(IndexPriceUpdate {
    instrument_id: InstrumentId,
    value: Price,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(IndexPriceUpdate, "IndexPriceUpdate", "Index price.", {
    (instrument_id: InstrumentId) (value: Price) (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(FundingRateUpdate { instrument_id: InstrumentId, rate: Price, interval: Option<u32>, next_funding_ns: Option<UnixNanos>, ts_event: UnixNanos, ts_init: UnixNanos });
py_struct!(FundingRateUpdate, "FundingRateUpdate", "Funding rate of a perpetual.", {
    (instrument_id: InstrumentId) (rate: Price as DecPrice) (interval: Option<u32>)
    (next_funding_ns: Option<UnixNanos>) (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(InstrumentClose {
    instrument_id: InstrumentId,
    close_price: Price,
    close_type: InstrumentCloseType,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(InstrumentClose, "InstrumentClose", "Close price of an instrument.", {
    (instrument_id: InstrumentId) (close_price: Price) (close_type: InstrumentCloseType)
    (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(LiquidationOrder {
    instrument_id: InstrumentId,
    side: OrderSide,
    price: Price,
    average_price: Price,
    original_quantity: Quantity,
    filled_quantity: Quantity,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(LiquidationOrder, "LiquidationOrder", "A liquidation published by the venue (jarvis extension).", {
    (instrument_id: InstrumentId) (side: OrderSide) (price: Price) (average_price: Price)
    (original_quantity: Quantity) (filled_quantity: Quantity) (ts_event: UnixNanos) (ts_init: UnixNanos)
});

// ---- account ------------------------------------------------------------------------------

from_parts!(AccountBalance { total: Money, locked: Money, free: Money, currency: Currency });
impl PyStruct for AccountBalance {
    const NAME: &'static str = "AccountBalance";
    const DOC: &'static str = "total == locked + free, one currency.";
    fn fields() -> Vec<FieldSpec> {
        vec![
            FieldSpec { name: "total", default: <Money as PyField>::default_py },
            FieldSpec { name: "locked", default: <Money as PyField>::default_py },
            FieldSpec { name: "free", default: <Money as PyField>::default_py },
        ]
    }
    fn write(&self, obj: &Bound<'_, PyAny>) -> PyResult<()> {
        obj.setattr("total", self.total.to_py(obj.py())?)?;
        obj.setattr("locked", self.locked.to_py(obj.py())?)?;
        obj.setattr("free", self.free.to_py(obj.py())?)?;
        Ok(())
    }
    fn read(obj: &Bound<'_, PyAny>) -> PyResult<Self> {
        let total = Money::from_py(&obj.getattr("total")?, "total")?;
        let locked = Money::from_py(&obj.getattr("locked")?, "locked")?;
        let free = Money::from_py(&obj.getattr("free")?, "free")?;
        check(AccountBalance::new(total, locked, free), "AccountBalance")
    }
}
from_parts!(MarginBalance { initial: Money, maintenance: Money, currency: Currency, instrument_id: Option<InstrumentId> });
py_struct!(MarginBalance, "MarginBalance", "Initial and maintenance margin.", {
    (initial: Money) (maintenance: Money) (currency: Currency) (instrument_id: Option<InstrumentId>)
});
from_parts!(AccountState { account_id: AccountId, account_type: AccountType, base_currency: Option<Currency>, balances: FixedVec<AccountBalance>, margins: FixedVec<MarginBalance>, is_reported: bool, event_id: Uuid4, ts_event: UnixNanos, ts_init: UnixNanos });
py_struct!(AccountState, "AccountState", "Snapshot of an account's balances and margins.", {
    (account_id: AccountId) (account_type: AccountType) (base_currency: Option<Currency>)
    (balances: FixedVec<AccountBalance>) (margins: FixedVec<MarginBalance>) (is_reported: bool)
    (event_id: Uuid4) (ts_event: UnixNanos) (ts_init: UnixNanos)
});

// ---- order events ---------------------------------------------------------------------------

from_parts!(OrderEventHeader { trader_id: TraderId, strategy_id: StrategyId, instrument_id: InstrumentId, client_order_id: ClientOrderId, event_id: Uuid4, ts_event: UnixNanos, ts_init: UnixNanos, causation_id: Option<Uuid4> });
py_struct!(OrderEventHeader, "OrderEventHeader", "The fields every order event carries.", {
    (trader_id: TraderId) (strategy_id: StrategyId) (instrument_id: InstrumentId)
    (client_order_id: ClientOrderId) (event_id: Uuid4) (ts_event: UnixNanos)
    (ts_init: UnixNanos) (causation_id: Option<Uuid4>)
});

macro_rules! order_event {
    ($t:ident, $doc:literal { $($f:ident : $ty:ty),* $(,)? }) => {
        from_parts!($t { header: OrderEventHeader $(, $f: $ty)* });
        py_struct!($t, stringify!($t), $doc, { (flat header: OrderEventHeader) $(($f: $ty))* });
    };
}

order_event!(OrderInitialized, "An order was created." {
    order_side: OrderSide, order_type: OrderType, quantity: Quantity, time_in_force: TimeInForce,
    post_only: bool, reduce_only: bool, quote_quantity: bool, reconciliation: bool,
    price: Option<Price>, activation_price: Option<Price>, trigger_price: Option<Price>,
    trigger_type: Option<TriggerType>, limit_offset: Option<Price>, trailing_offset: Option<Price>,
    trailing_offset_type: Option<TrailingOffsetType>, expire_time: Option<UnixNanos>,
    display_qty: Option<Quantity>, emulation_trigger: Option<TriggerType>,
    trigger_instrument_id: Option<InstrumentId>, contingency_type: Option<ContingencyType>,
    order_list_id: Option<OrderListId>, parent_order_id: Option<ClientOrderId>,
    exec_algorithm_id: Option<ExecAlgorithmId>, exec_spawn_id: Option<ClientOrderId>,
});
order_event!(OrderDenied, "The risk gates denied an order." { reason: Reason });
order_event!(OrderEmulated, "An order is held for emulation." {});
order_event!(OrderReleased, "An emulated order was released." { released_price: Price });
order_event!(OrderSubmitted, "An order was sent to the venue." { account_id: AccountId });
order_event!(OrderAccepted, "The venue accepted an order." {
    venue_order_id: VenueOrderId, account_id: AccountId, reconciliation: bool,
});
order_event!(OrderRejected, "The venue rejected an order." {
    account_id: AccountId, reason: Reason, reconciliation: bool, due_post_only: bool,
});
order_event!(OrderCanceled, "An order was canceled." {
    venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>,
    reason: Option<Reason>, reconciliation: bool,
});
order_event!(OrderExpired, "An order expired." {
    venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reconciliation: bool,
});
order_event!(OrderTriggered, "A conditional order triggered." {
    venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, reconciliation: bool,
});
order_event!(OrderPendingUpdate, "A modify request is in flight." {
    account_id: AccountId, venue_order_id: Option<VenueOrderId>, reconciliation: bool,
});
order_event!(OrderPendingCancel, "A cancel request is in flight." {
    account_id: AccountId, venue_order_id: Option<VenueOrderId>, reconciliation: bool,
});
order_event!(OrderModifyRejected, "The venue refused a modify." {
    reason: Reason, venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>,
    reconciliation: bool,
});
order_event!(OrderCancelRejected, "The venue refused a cancel." {
    reason: Reason, venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>,
    reconciliation: bool,
});
order_event!(OrderUpdated, "An order was modified." {
    venue_order_id: Option<VenueOrderId>, account_id: Option<AccountId>, quantity: Quantity,
    price: Option<Price>, trigger_price: Option<Price>, protection_price: Option<Price>,
    is_quote_quantity: bool, reconciliation: bool,
});
order_event!(OrderFilled, "An order (partially) filled." {
    venue_order_id: VenueOrderId, account_id: AccountId, trade_id: TradeId,
    order_side: OrderSide, order_type: OrderType, last_qty: Quantity, last_px: Price, currency: Currency,
    liquidity_side: LiquiditySide, reconciliation: bool, position_id: Option<PositionId>,
    commission: Option<Money>, info_flags: FillInfoFlags,
});
order_event!(OrderFillVoided, "The venue voided an earlier fill." {
    venue_order_id: VenueOrderId, account_id: AccountId, correction_id: TradeId,
    trade_id: TradeId, voided_qty: Quantity, commission_voided: Option<Money>, order_side: OrderSide,
    order_type: OrderType, last_px: Price, currency: Currency, liquidity_side: LiquiditySide,
    position_id: Option<PositionId>, reason: Option<Reason>, reconciliation: bool, is_reopened: bool,
    info_flags: FillInfoFlags,
});

// ---- position events ------------------------------------------------------------------------

from_parts!(PositionEventHeader {
    trader_id: TraderId,
    strategy_id: StrategyId,
    instrument_id: InstrumentId,
    position_id: PositionId,
    account_id: AccountId,
    event_id: Uuid4,
    ts_event: UnixNanos,
    ts_init: UnixNanos
});
py_struct!(PositionEventHeader, "PositionEventHeader", "The fields every position event carries.", {
    (trader_id: TraderId) (strategy_id: StrategyId) (instrument_id: InstrumentId)
    (position_id: PositionId) (account_id: AccountId) (event_id: Uuid4)
    (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(PositionState { opening_order_id: ClientOrderId, entry: OrderSide, side: PositionSide, signed_qty: i64, quantity: Quantity, peak_quantity: Quantity, last_qty: Quantity, last_px: Price, currency: Currency, avg_px_open: Price, avg_px_close: Option<Price>, realized_return: i64, realized_pnl: Money, unrealized_pnl: Option<Money> });
py_struct!(PositionState, "PositionState", "A position after an event.", {
    (opening_order_id: ClientOrderId) (entry: OrderSide) (side: PositionSide) (signed_qty: i64 as Dec9)
    (quantity: Quantity) (peak_quantity: Quantity) (last_qty: Quantity) (last_px: Price) (currency: Currency)
    (avg_px_open: Price) (avg_px_close: Option<Price>) (realized_return: i64 as Dec9) (realized_pnl: Money)
    (unrealized_pnl: Option<Money>)
});
from_parts!(PositionOpened { header: PositionEventHeader, state: PositionState });
py_struct!(PositionOpened, "PositionOpened", "A position opened.", {
    (flat header: PositionEventHeader) (flat state: PositionState)
});
from_parts!(PositionChanged {
    header: PositionEventHeader,
    state: PositionState,
    ts_opened: UnixNanos
});
py_struct!(PositionChanged, "PositionChanged", "A position changed.", {
    (flat header: PositionEventHeader) (flat state: PositionState) (ts_opened: UnixNanos)
});
from_parts!(PositionClosed {
    header: PositionEventHeader,
    state: PositionState,
    closing_order_id: ClientOrderId,
    ts_opened: UnixNanos,
    ts_closed: UnixNanos,
    duration: DurationNanos
});
py_struct!(PositionClosed, "PositionClosed", "A position closed.", {
    (flat header: PositionEventHeader) (flat state: PositionState) (closing_order_id: ClientOrderId)
    (ts_opened: UnixNanos) (ts_closed: UnixNanos) (duration: DurationNanos)
});
from_parts!(PositionAdjusted { header: PositionEventHeader, adjustment_type: PositionAdjustmentType, quantity_change: i64, pnl_change: Money, reason: Option<FixedString<96>> });
py_struct!(PositionAdjusted, "PositionAdjusted", "A position was adjusted.", {
    (flat header: PositionEventHeader) (adjustment_type: PositionAdjustmentType)
    (quantity_change: i64 as Dec9) (pnl_change: Money) (reason: Option<FixedString<96>>)
});

// ---- kernel inputs --------------------------------------------------------------------------

from_parts!(TimerFired { key: kernel_core::clock::TimerKey, deadline: UnixNanos });
py_struct!(TimerFired, "TimerFired", "A timer deadline reached (a recorded input).", {
    (key: kernel_core::clock::TimerKey) (deadline: UnixNanos)
});
from_parts!(BatchEnd { ts: UnixNanos });
py_struct!(BatchEnd, "BatchEnd", "End of one drained batch of inputs.", { (ts: UnixNanos) });
from_parts!(NodeLifecycle {
    from: NodeState,
    to: NodeState,
    reason: LifecycleReason,
    ts: UnixNanos
});
py_struct!(NodeLifecycle, "NodeLifecycle", "A node lifecycle transition.", {
    (from: NodeState) (to: NodeState) (reason: LifecycleReason) (ts: UnixNanos)
});
from_parts!(StrategyError {
    strategy_index: u16,
    kind: StrategyErrorKind,
    message_hash: u64,
    ts_init: UnixNanos
});
py_struct!(StrategyError, "StrategyError", "A strategy callback failed or overran.", {
    (strategy_index: u16) (kind: StrategyErrorKind) (message_hash: u64) (ts_init: UnixNanos)
});
from_parts!(Shutdown { mode: StopMode });
py_struct!(Shutdown, "Shutdown", "A shutdown request.", { (mode: StopMode) });
from_parts!(RateLimitFeedback {
    used_weight_1m: u32,
    order_count_10s: u32,
    order_count_1m: u32,
    ts_init: UnixNanos
});
py_struct!(RateLimitFeedback, "RateLimitFeedback", "The venue's count of the rate limit windows.", {
    (used_weight_1m: u32) (order_count_10s: u32) (order_count_1m: u32) (ts_init: UnixNanos)
});

// ---- outputs --------------------------------------------------------------------------------

from_parts!(FeatureUpdate { feature_id: u32, value: i64, ts_event: UnixNanos, ts_init: UnixNanos });
py_struct!(FeatureUpdate, "FeatureUpdate", "A kernel feature value (an output record keyed by its input).", {
    (feature_id: u32) (value: i64 as Dec9) (ts_event: UnixNanos) (ts_init: UnixNanos)
});
from_parts!(StrategyRecord { strategy_index: u16, tag: RecordTag, value: i64, ts_init: UnixNanos });
py_struct!(StrategyRecord, "StrategyRecord", "A value a strategy recorded with ctx.record(tag, value).", {
    (strategy_index: u16) (tag: RecordTag) (value: i64 as Dec9) (ts_init: UnixNanos)
});
from_parts!(SubmitOrder { strategy_index: u16, client_order_id: ClientOrderId, instrument_id: InstrumentId, order_side: OrderSide, order_type: OrderType, quantity: Quantity, price: Option<Price>, time_in_force: TimeInForce, post_only: bool, reduce_only: bool, expire_time: Option<UnixNanos>, ts_init: UnixNanos });
py_struct!(SubmitOrder, "SubmitOrder", "A new order sent to the venue (a command).", {
    (strategy_index: u16) (client_order_id: ClientOrderId) (instrument_id: InstrumentId)
    (order_side: OrderSide) (order_type: OrderType) (quantity: Quantity) (price: Option<Price>)
    (time_in_force: TimeInForce) (post_only: bool) (reduce_only: bool) (expire_time: Option<UnixNanos>)
    (ts_init: UnixNanos)
});
from_parts!(ModifyOrder { strategy_index: u16, client_order_id: ClientOrderId, instrument_id: InstrumentId, venue_order_id: Option<VenueOrderId>, quantity: Quantity, price: Price, ts_init: UnixNanos });
py_struct!(ModifyOrder, "ModifyOrder", "A LIMIT order's new quantity and price.", {
    (strategy_index: u16) (client_order_id: ClientOrderId) (instrument_id: InstrumentId)
    (venue_order_id: Option<VenueOrderId>) (quantity: Quantity) (price: Price) (ts_init: UnixNanos)
});
from_parts!(CancelOrder { strategy_index: u16, client_order_id: ClientOrderId, instrument_id: InstrumentId, venue_order_id: Option<VenueOrderId>, ts_init: UnixNanos });
py_struct!(CancelOrder, "CancelOrder", "A cancel request for one order.", {
    (strategy_index: u16) (client_order_id: ClientOrderId) (instrument_id: InstrumentId)
    (venue_order_id: Option<VenueOrderId>) (ts_init: UnixNanos)
});
from_parts!(CancelAllOrders {
    strategy_index: u16,
    instrument_id: InstrumentId,
    ts_init: UnixNanos
});
py_struct!(CancelAllOrders, "CancelAllOrders", "A cancel request for every open order of an instrument.", {
    (strategy_index: u16) (instrument_id: InstrumentId) (ts_init: UnixNanos)
});
from_parts!(CountdownCancelAll {
    instrument_id: InstrumentId,
    countdown_ms: u32,
    ts_init: UnixNanos
});
py_struct!(CountdownCancelAll, "CountdownCancelAll", "The venue-side dead man's switch for an instrument: renewed, or disarmed with 0.", {
    (instrument_id: InstrumentId) (countdown_ms: u32) (ts_init: UnixNanos)
});

// ---- order book deltas (a batch; its summary fields come from the last delta) ---------------

pub(crate) const DELTAS_FIELDS: &[&str] = &["instrument_id", "deltas"];
pub(crate) const DELTAS_DERIVED: &[&str] = &["flags", "sequence", "ts_event", "ts_init"];

pub(crate) fn deltas_write(d: &OrderBookDeltas, obj: &Bound<'_, PyAny>) -> PyResult<()> {
    let py = obj.py();
    obj.setattr("instrument_id", d.instrument_id.to_py(py)?)?;
    obj.setattr("deltas", d.deltas.to_py(py)?)?;
    obj.setattr("flags", d.flags.to_py(py)?)?;
    obj.setattr("sequence", d.sequence.to_py(py)?)?;
    obj.setattr("ts_event", d.ts_event.to_py(py)?)?;
    obj.setattr("ts_init", d.ts_init.to_py(py)?)?;
    Ok(())
}

pub(crate) fn deltas_read(obj: &Bound<'_, PyAny>) -> PyResult<OrderBookDeltas> {
    let instrument_id = InstrumentId::from_py(&obj.getattr("instrument_id")?, "instrument_id")?;
    let deltas = <FixedVec<OrderBookDelta>>::from_py(&obj.getattr("deltas")?, "deltas")?;
    if deltas.iter().any(|d| d.instrument_id != instrument_id) {
        return Err(pyo3::exceptions::PyValueError::new_err(
            "OrderBookDeltas: every delta must be for instrument_id",
        ));
    }
    check(OrderBookDeltas::new(instrument_id, deltas), "OrderBookDeltas")
}

// ---- instruments ----------------------------------------------------------------------------

/// The three instrument classes share these attributes.
const INSTRUMENT_COMMON: &[&str] = &[
    "price_precision",
    "size_precision",
    "price_increment",
    "size_increment",
    "multiplier",
    "lot_size",
    "margin_init",
    "margin_maint",
    "max_quantity",
    "min_quantity",
    "max_notional",
    "min_notional",
    "max_price",
    "min_price",
    "ts_event",
    "ts_init",
];

pub(crate) struct InstrumentClassSpec {
    pub name: &'static str,
    pub doc: &'static str,
    pub leading: &'static [&'static str],
}

pub(crate) const INSTRUMENT_CLASSES: &[InstrumentClassSpec] = &[
    InstrumentClassSpec {
        name: "CurrencyPair",
        doc: "Spot pair.",
        leading: &["id", "raw_symbol", "base_currency", "quote_currency"],
    },
    InstrumentClassSpec {
        name: "CryptoPerpetual",
        doc: "Perpetual swap.",
        leading: &[
            "id",
            "raw_symbol",
            "base_currency",
            "quote_currency",
            "settlement_currency",
            "is_inverse",
        ],
    },
    InstrumentClassSpec {
        name: "CryptoFuture",
        doc: "Dated future.",
        leading: &[
            "id",
            "raw_symbol",
            "underlying",
            "quote_currency",
            "settlement_currency",
            "is_inverse",
            "activation_ns",
            "expiration_ns",
        ],
    },
];

pub(crate) fn instrument_fields(spec: &InstrumentClassSpec) -> Vec<FieldSpec> {
    let optional: fn(Python<'_>) -> Option<Py<PyAny>> = |py| Some(py.None());
    let required: fn(Python<'_>) -> Option<Py<PyAny>> = |_| None;
    let flag: fn(Python<'_>) -> Option<Py<PyAny>> = <bool as PyField>::default_py;
    let mut out = Vec::new();
    for name in spec.leading.iter().chain(INSTRUMENT_COMMON) {
        let default = match *name {
            "lot_size" | "max_quantity" | "min_quantity" | "max_notional" | "min_notional"
            | "max_price" | "min_price" => optional,
            "is_inverse" => flag,
            _ => required,
        };
        out.push(FieldSpec { name, default });
    }
    out
}

pub(crate) fn instrument_class_name(i: &Instrument) -> &'static str {
    match i.kind {
        InstrumentKind::CurrencyPair => "CurrencyPair",
        InstrumentKind::CryptoPerpetual { .. } => "CryptoPerpetual",
        InstrumentKind::CryptoFuture { .. } => "CryptoFuture",
    }
}

pub(crate) fn instrument_write(i: &Instrument, obj: &Bound<'_, PyAny>) -> PyResult<()> {
    let py = obj.py();
    obj.setattr("id", i.id.to_py(py)?)?;
    obj.setattr("raw_symbol", i.raw_symbol.to_py(py)?)?;
    match i.kind {
        InstrumentKind::CurrencyPair => {
            obj.setattr("base_currency", i.base_currency.to_py(py)?)?;
        }
        InstrumentKind::CryptoPerpetual { settlement_currency, is_inverse } => {
            obj.setattr("base_currency", i.base_currency.to_py(py)?)?;
            obj.setattr("settlement_currency", settlement_currency.to_py(py)?)?;
            obj.setattr("is_inverse", is_inverse.to_py(py)?)?;
        }
        InstrumentKind::CryptoFuture {
            underlying,
            settlement_currency,
            is_inverse,
            activation_ns,
            expiration_ns,
        } => {
            obj.setattr("underlying", underlying.to_py(py)?)?;
            obj.setattr("settlement_currency", settlement_currency.to_py(py)?)?;
            obj.setattr("is_inverse", is_inverse.to_py(py)?)?;
            obj.setattr("activation_ns", activation_ns.to_py(py)?)?;
            obj.setattr("expiration_ns", expiration_ns.to_py(py)?)?;
        }
    }
    obj.setattr("quote_currency", i.quote_currency.to_py(py)?)?;
    obj.setattr("price_precision", i.price_precision.to_py(py)?)?;
    obj.setattr("size_precision", i.size_precision.to_py(py)?)?;
    obj.setattr("price_increment", i.price_increment.to_py(py)?)?;
    obj.setattr("size_increment", i.size_increment.to_py(py)?)?;
    obj.setattr("multiplier", i.multiplier.to_py(py)?)?;
    obj.setattr("lot_size", i.lot_size.to_py(py)?)?;
    obj.setattr("margin_init", DecQuantity(i.margin_init).to_py(py)?)?;
    obj.setattr("margin_maint", DecQuantity(i.margin_maint).to_py(py)?)?;
    obj.setattr("max_quantity", i.limits.max_quantity.to_py(py)?)?;
    obj.setattr("min_quantity", i.limits.min_quantity.to_py(py)?)?;
    obj.setattr("max_notional", i.limits.max_notional.to_py(py)?)?;
    obj.setattr("min_notional", i.limits.min_notional.to_py(py)?)?;
    obj.setattr("max_price", i.limits.max_price.to_py(py)?)?;
    obj.setattr("min_price", i.limits.min_price.to_py(py)?)?;
    obj.setattr("ts_event", i.ts_event.to_py(py)?)?;
    obj.setattr("ts_init", i.ts_init.to_py(py)?)?;
    Ok(())
}

pub(crate) fn instrument_read(obj: &Bound<'_, PyAny>, class: &str) -> PyResult<Instrument> {
    let get = |name: &str| obj.getattr(name);
    let kind = match class {
        "CurrencyPair" => InstrumentKind::CurrencyPair,
        "CryptoPerpetual" => InstrumentKind::CryptoPerpetual {
            settlement_currency: Currency::from_py(
                &get("settlement_currency")?,
                "settlement_currency",
            )?,
            is_inverse: bool::from_py(&get("is_inverse")?, "is_inverse")?,
        },
        "CryptoFuture" => InstrumentKind::CryptoFuture {
            underlying: Currency::from_py(&get("underlying")?, "underlying")?,
            settlement_currency: Currency::from_py(
                &get("settlement_currency")?,
                "settlement_currency",
            )?,
            is_inverse: bool::from_py(&get("is_inverse")?, "is_inverse")?,
            activation_ns: UnixNanos::from_py(&get("activation_ns")?, "activation_ns")?,
            expiration_ns: UnixNanos::from_py(&get("expiration_ns")?, "expiration_ns")?,
        },
        _ => return Err(PyTypeError::new_err(format!("{class} is not an instrument class"))),
    };
    let base_currency = match kind {
        InstrumentKind::CryptoFuture { underlying, .. } => Some(underlying),
        _ => <Option<Currency>>::from_py(&get("base_currency")?, "base_currency")?,
    };
    let limits = InstrumentLimits {
        max_quantity: <Option<Quantity>>::from_py(&get("max_quantity")?, "max_quantity")?,
        min_quantity: <Option<Quantity>>::from_py(&get("min_quantity")?, "min_quantity")?,
        max_notional: <Option<Money>>::from_py(&get("max_notional")?, "max_notional")?,
        min_notional: <Option<Money>>::from_py(&get("min_notional")?, "min_notional")?,
        max_price: <Option<Price>>::from_py(&get("max_price")?, "max_price")?,
        min_price: <Option<Price>>::from_py(&get("min_price")?, "min_price")?,
    };
    let price_increment = Price::from_py(&get("price_increment")?, "price_increment")?;
    let size_increment = Quantity::from_py(&get("size_increment")?, "size_increment")?;
    let price_precision = u8::from_py(&get("price_precision")?, "price_precision")?;
    let size_precision = u8::from_py(&get("size_precision")?, "size_precision")?;
    if price_precision != price_increment.precision()
        || size_precision != size_increment.precision()
    {
        return Err(crate::common::status_error(class, kernel_core::Status::InvalidArgument));
    }
    let instrument: KResult<Instrument> = Instrument::new(
        InstrumentId::from_py(&get("id")?, "id")?,
        Symbol::from_py(&get("raw_symbol")?, "raw_symbol")?,
        AssetClass::Cryptocurrency,
        kind,
        base_currency,
        Currency::from_py(&get("quote_currency")?, "quote_currency")?,
        price_increment,
        size_increment,
        Quantity::from_py(&get("multiplier")?, "multiplier")?,
        <Option<Quantity>>::from_py(&get("lot_size")?, "lot_size")?,
        limits,
        DecQuantity::from_py(&get("margin_init")?, "margin_init")?.0,
        DecQuantity::from_py(&get("margin_maint")?, "margin_maint")?.0,
        UnixNanos::from_py(&get("ts_event")?, "ts_event")?,
        UnixNanos::from_py(&get("ts_init")?, "ts_init")?,
    );
    check(instrument, class)
}

// ---- events and outputs as Python objects ---------------------------------------------------

/// The Python class name of an event.
pub(crate) fn event_class_name(e: &Event) -> &'static str {
    match e {
        Event::TradeTick(_) => "TradeTick",
        Event::QuoteTick(_) => "QuoteTick",
        Event::OrderBookDeltas(_) => "OrderBookDeltas",
        Event::Bar(_) => "Bar",
        Event::MarkPriceUpdate(_) => "MarkPriceUpdate",
        Event::IndexPriceUpdate(_) => "IndexPriceUpdate",
        Event::FundingRateUpdate(_) => "FundingRateUpdate",
        Event::InstrumentStatus(_) => "InstrumentStatus",
        Event::InstrumentClose(_) => "InstrumentClose",
        Event::LiquidationOrder(_) => "LiquidationOrder",
        Event::Instrument(i) => instrument_class_name(i),
        Event::Order(o) => o.name(),
        Event::AccountState(_) => "AccountState",
        Event::RateLimitFeedback(_) => "RateLimitFeedback",
        Event::TimerFired(_) => "TimerFired",
        Event::BatchEnd(_) => "BatchEnd",
        Event::NodeLifecycle(_) => "NodeLifecycle",
        Event::StrategyError(_) => "StrategyError",
        Event::Shutdown(_) => "Shutdown",
    }
}

pub(crate) fn event_to_py<'py>(py: Python<'py>, e: &Event) -> PyResult<Bound<'py, PyAny>> {
    match e {
        Event::TradeTick(v) => make(py, v),
        Event::QuoteTick(v) => make(py, v),
        Event::OrderBookDeltas(v) => {
            let obj = crate::model::new_instance(py, "OrderBookDeltas")?;
            deltas_write(v, &obj)?;
            Ok(obj)
        }
        Event::Bar(v) => make(py, v),
        Event::MarkPriceUpdate(v) => make(py, v),
        Event::IndexPriceUpdate(v) => make(py, v),
        Event::FundingRateUpdate(v) => make(py, v),
        Event::InstrumentStatus(v) => make(py, v),
        Event::InstrumentClose(v) => make(py, v),
        Event::LiquidationOrder(v) => make(py, v),
        Event::Instrument(v) => {
            let obj = crate::model::new_instance(py, instrument_class_name(v))?;
            instrument_write(v, &obj)?;
            Ok(obj)
        }
        Event::Order(v) => order_event_to_py(py, v),
        Event::AccountState(v) => make(py, v),
        Event::RateLimitFeedback(v) => make(py, v),
        Event::TimerFired(v) => make(py, v),
        Event::BatchEnd(v) => make(py, v),
        Event::NodeLifecycle(v) => make(py, v),
        Event::StrategyError(v) => make(py, v),
        Event::Shutdown(v) => make(py, v),
    }
}

pub(crate) fn order_event_to_py<'py>(
    py: Python<'py>,
    e: &OrderEvent,
) -> PyResult<Bound<'py, PyAny>> {
    match e {
        OrderEvent::Initialized(v) => make(py, v),
        OrderEvent::Denied(v) => make(py, v),
        OrderEvent::Emulated(v) => make(py, v),
        OrderEvent::Released(v) => make(py, v),
        OrderEvent::Submitted(v) => make(py, v),
        OrderEvent::Accepted(v) => make(py, v),
        OrderEvent::Rejected(v) => make(py, v),
        OrderEvent::Canceled(v) => make(py, v),
        OrderEvent::Expired(v) => make(py, v),
        OrderEvent::Triggered(v) => make(py, v),
        OrderEvent::PendingUpdate(v) => make(py, v),
        OrderEvent::PendingCancel(v) => make(py, v),
        OrderEvent::ModifyRejected(v) => make(py, v),
        OrderEvent::CancelRejected(v) => make(py, v),
        OrderEvent::Updated(v) => make(py, v),
        OrderEvent::Filled(v) => make(py, v),
        OrderEvent::FillVoided(v) => make(py, v),
    }
}

pub(crate) fn position_event_to_py<'py>(
    py: Python<'py>,
    e: &PositionEvent,
) -> PyResult<Bound<'py, PyAny>> {
    match e {
        PositionEvent::Opened(v) => make(py, v),
        PositionEvent::Changed(v) => make(py, v),
        PositionEvent::Closed(v) => make(py, v),
        PositionEvent::Adjusted(v) => make(py, v),
    }
}

pub(crate) fn output_class_name(o: &Output) -> &'static str {
    o.name()
}

pub(crate) fn output_to_py<'py>(py: Python<'py>, o: &Output) -> PyResult<Bound<'py, PyAny>> {
    match o {
        Output::FeatureUpdate(v) => make(py, v),
        Output::StrategyRecord(v) => make(py, v),
        Output::SubmitOrder(v) => make(py, v),
        Output::ModifyOrder(v) => make(py, v),
        Output::CancelOrder(v) => make(py, v),
        Output::CancelAllOrders(v) => make(py, v),
        Output::OrderDenied(v) => make(py, v),
        Output::CountdownCancelAll(v) => make(py, v),
    }
}

/// The input event a Python object is, by its class.
pub(crate) fn event_from_py(obj: &Bound<'_, PyAny>) -> PyResult<Event> {
    let class = crate::model::class_name(obj)?;
    Ok(match class.as_str() {
        "TradeTick" => Event::TradeTick(TradeTick::read(obj)?),
        "QuoteTick" => Event::QuoteTick(QuoteTick::read(obj)?),
        "OrderBookDeltas" => Event::OrderBookDeltas(deltas_read(obj)?),
        "Bar" => Event::Bar(Bar::read(obj)?),
        "MarkPriceUpdate" => Event::MarkPriceUpdate(MarkPriceUpdate::read(obj)?),
        "IndexPriceUpdate" => Event::IndexPriceUpdate(IndexPriceUpdate::read(obj)?),
        "FundingRateUpdate" => Event::FundingRateUpdate(FundingRateUpdate::read(obj)?),
        "InstrumentStatus" => Event::InstrumentStatus(InstrumentStatus::read(obj)?),
        "InstrumentClose" => Event::InstrumentClose(InstrumentClose::read(obj)?),
        "LiquidationOrder" => Event::LiquidationOrder(LiquidationOrder::read(obj)?),
        "CurrencyPair" | "CryptoPerpetual" | "CryptoFuture" => {
            Event::Instrument(instrument_read(obj, &class)?)
        }
        "AccountState" => Event::AccountState(AccountState::read(obj)?),
        "RateLimitFeedback" => Event::RateLimitFeedback(RateLimitFeedback::read(obj)?),
        "TimerFired" => Event::TimerFired(TimerFired::read(obj)?),
        "BatchEnd" => Event::BatchEnd(BatchEnd::read(obj)?),
        "NodeLifecycle" => Event::NodeLifecycle(NodeLifecycle::read(obj)?),
        "StrategyError" => Event::StrategyError(StrategyError::read(obj)?),
        "Shutdown" => Event::Shutdown(Shutdown::read(obj)?),
        _ => match order_event_from_py(obj, &class)? {
            Some(e) => Event::Order(e),
            None => {
                return Err(type_error(
                    "event",
                    "an event (TradeTick, QuoteTick, OrderFilled, ...)",
                    obj,
                ))
            }
        },
    })
}

pub(crate) fn order_event_from_py(
    obj: &Bound<'_, PyAny>,
    class: &str,
) -> PyResult<Option<OrderEvent>> {
    Ok(Some(match class {
        "OrderInitialized" => OrderEvent::Initialized(OrderInitialized::read(obj)?),
        "OrderDenied" => OrderEvent::Denied(OrderDenied::read(obj)?),
        "OrderEmulated" => OrderEvent::Emulated(OrderEmulated::read(obj)?),
        "OrderReleased" => OrderEvent::Released(OrderReleased::read(obj)?),
        "OrderSubmitted" => OrderEvent::Submitted(OrderSubmitted::read(obj)?),
        "OrderAccepted" => OrderEvent::Accepted(OrderAccepted::read(obj)?),
        "OrderRejected" => OrderEvent::Rejected(OrderRejected::read(obj)?),
        "OrderCanceled" => OrderEvent::Canceled(OrderCanceled::read(obj)?),
        "OrderExpired" => OrderEvent::Expired(OrderExpired::read(obj)?),
        "OrderTriggered" => OrderEvent::Triggered(OrderTriggered::read(obj)?),
        "OrderPendingUpdate" => OrderEvent::PendingUpdate(OrderPendingUpdate::read(obj)?),
        "OrderPendingCancel" => OrderEvent::PendingCancel(OrderPendingCancel::read(obj)?),
        "OrderModifyRejected" => OrderEvent::ModifyRejected(OrderModifyRejected::read(obj)?),
        "OrderCancelRejected" => OrderEvent::CancelRejected(OrderCancelRejected::read(obj)?),
        "OrderUpdated" => OrderEvent::Updated(OrderUpdated::read(obj)?),
        "OrderFilled" => OrderEvent::Filled(OrderFilled::read(obj)?),
        "OrderFillVoided" => OrderEvent::FillVoided(OrderFillVoided::read(obj)?),
        _ => return Ok(None),
    }))
}

/// The output a Python object is, by its class; `OrderDenied` is written as the input kind.
pub(crate) fn output_from_py(obj: &Bound<'_, PyAny>) -> PyResult<Option<Output>> {
    let class = crate::model::class_name(obj)?;
    Ok(Some(match class.as_str() {
        "FeatureUpdate" => Output::FeatureUpdate(FeatureUpdate::read(obj)?),
        "StrategyRecord" => Output::StrategyRecord(StrategyRecord::read(obj)?),
        "SubmitOrder" => Output::SubmitOrder(SubmitOrder::read(obj)?),
        "ModifyOrder" => Output::ModifyOrder(ModifyOrder::read(obj)?),
        "CancelOrder" => Output::CancelOrder(CancelOrder::read(obj)?),
        "CancelAllOrders" => Output::CancelAllOrders(CancelAllOrders::read(obj)?),
        "CountdownCancelAll" => Output::CountdownCancelAll(CountdownCancelAll::read(obj)?),
        _ => return Ok(None),
    }))
}

/// Validates a freshly constructed Python event and writes its attributes back normalized
/// (`"65000.1"` becomes a `Price`, an enum name its member, ...).
pub(crate) fn normalize(obj: &Bound<'_, PyAny>) -> PyResult<()> {
    let class = crate::model::class_name(obj)?;
    match class.as_str() {
        "OrderBookDeltas" => return deltas_write(&deltas_read(obj)?, obj),
        "CurrencyPair" | "CryptoPerpetual" | "CryptoFuture" => {
            return instrument_write(&instrument_read(obj, &class)?, obj)
        }
        "AccountBalance" => return AccountBalance::read(obj)?.write(obj),
        "MarginBalance" => return MarginBalance::read(obj)?.write(obj),
        "OrderBookDelta" => return OrderBookDelta::read(obj)?.write(obj),
        "PositionOpened" => return PositionOpened::read(obj)?.write(obj),
        "PositionChanged" => return PositionChanged::read(obj)?.write(obj),
        "PositionClosed" => return PositionClosed::read(obj)?.write(obj),
        "PositionAdjusted" => return PositionAdjusted::read(obj)?.write(obj),
        _ => {}
    }
    if let Some(o) = output_from_py(obj)? {
        return match o {
            Output::FeatureUpdate(v) => v.write(obj),
            Output::StrategyRecord(v) => v.write(obj),
            Output::SubmitOrder(v) => v.write(obj),
            Output::ModifyOrder(v) => v.write(obj),
            Output::CancelOrder(v) => v.write(obj),
            Output::CancelAllOrders(v) => v.write(obj),
            Output::OrderDenied(v) => v.write(obj),
            Output::CountdownCancelAll(v) => v.write(obj),
        };
    }
    let e = event_from_py(obj)?;
    match &e {
        Event::Order(o) => order_write(o, obj),
        Event::Instrument(i) => instrument_write(i, obj),
        Event::OrderBookDeltas(d) => deltas_write(d, obj),
        Event::TradeTick(v) => v.write(obj),
        Event::QuoteTick(v) => v.write(obj),
        Event::Bar(v) => v.write(obj),
        Event::MarkPriceUpdate(v) => v.write(obj),
        Event::IndexPriceUpdate(v) => v.write(obj),
        Event::FundingRateUpdate(v) => v.write(obj),
        Event::InstrumentStatus(v) => v.write(obj),
        Event::InstrumentClose(v) => v.write(obj),
        Event::LiquidationOrder(v) => v.write(obj),
        Event::AccountState(v) => v.write(obj),
        Event::RateLimitFeedback(v) => v.write(obj),
        Event::TimerFired(v) => v.write(obj),
        Event::BatchEnd(v) => v.write(obj),
        Event::NodeLifecycle(v) => v.write(obj),
        Event::StrategyError(v) => v.write(obj),
        Event::Shutdown(v) => v.write(obj),
    }
}

fn order_write(o: &OrderEvent, obj: &Bound<'_, PyAny>) -> PyResult<()> {
    match o {
        OrderEvent::Initialized(v) => v.write(obj),
        OrderEvent::Denied(v) => v.write(obj),
        OrderEvent::Emulated(v) => v.write(obj),
        OrderEvent::Released(v) => v.write(obj),
        OrderEvent::Submitted(v) => v.write(obj),
        OrderEvent::Accepted(v) => v.write(obj),
        OrderEvent::Rejected(v) => v.write(obj),
        OrderEvent::Canceled(v) => v.write(obj),
        OrderEvent::Expired(v) => v.write(obj),
        OrderEvent::Triggered(v) => v.write(obj),
        OrderEvent::PendingUpdate(v) => v.write(obj),
        OrderEvent::PendingCancel(v) => v.write(obj),
        OrderEvent::ModifyRejected(v) => v.write(obj),
        OrderEvent::CancelRejected(v) => v.write(obj),
        OrderEvent::Updated(v) => v.write(obj),
        OrderEvent::Filled(v) => v.write(obj),
        OrderEvent::FillVoided(v) => v.write(obj),
    }
}

/// `Kind field=value ...`: the event's line as `jarvis-rs dump` would print it.
pub(crate) fn event_text(obj: &Bound<'_, PyAny>) -> PyResult<String> {
    let class = crate::model::class_name(obj)?;
    let mut out = class.clone();
    let fields: Vec<String> = obj.getattr("fields")?.extract()?;
    for f in fields {
        let v = obj.getattr(f.as_str())?;
        let text = if v.is_instance_of::<PyList>() {
            format!("[{} items]", v.len()?)
        } else if v.is_instance_of::<PyString>() {
            v.extract::<String>()?
        } else if v.hasattr("value")? && v.hasattr("name")? && !v.hasattr("fields")? {
            v.getattr("name")?.extract::<String>()? // an enum member
        } else {
            v.str()?.to_str()?.to_string()
        };
        out.push(' ');
        out.push_str(&f);
        out.push('=');
        out.push_str(&text);
    }
    Ok(out)
}

/// The event class table: `(name, doc, fields)`, outputs included.
pub(crate) fn class_specs() -> Vec<(&'static str, &'static str, Vec<FieldSpec>)> {
    fn spec<T: PyStruct>() -> (&'static str, &'static str, Vec<FieldSpec>) {
        (T::NAME, T::DOC, T::fields())
    }
    vec![
        spec::<QuoteTick>(),
        spec::<TradeTick>(),
        spec::<Bar>(),
        spec::<OrderBookDelta>(),
        spec::<InstrumentStatus>(),
        spec::<MarkPriceUpdate>(),
        spec::<IndexPriceUpdate>(),
        spec::<FundingRateUpdate>(),
        spec::<InstrumentClose>(),
        spec::<LiquidationOrder>(),
        spec::<AccountBalance>(),
        spec::<MarginBalance>(),
        spec::<AccountState>(),
        spec::<OrderInitialized>(),
        spec::<OrderDenied>(),
        spec::<OrderEmulated>(),
        spec::<OrderReleased>(),
        spec::<OrderSubmitted>(),
        spec::<OrderAccepted>(),
        spec::<OrderRejected>(),
        spec::<OrderCanceled>(),
        spec::<OrderExpired>(),
        spec::<OrderTriggered>(),
        spec::<OrderPendingUpdate>(),
        spec::<OrderPendingCancel>(),
        spec::<OrderModifyRejected>(),
        spec::<OrderCancelRejected>(),
        spec::<OrderUpdated>(),
        spec::<OrderFilled>(),
        spec::<OrderFillVoided>(),
        spec::<PositionOpened>(),
        spec::<PositionChanged>(),
        spec::<PositionClosed>(),
        spec::<PositionAdjusted>(),
        spec::<TimerFired>(),
        spec::<BatchEnd>(),
        spec::<NodeLifecycle>(),
        spec::<StrategyError>(),
        spec::<Shutdown>(),
        spec::<RateLimitFeedback>(),
        spec::<FeatureUpdate>(),
        spec::<StrategyRecord>(),
        spec::<SubmitOrder>(),
        spec::<ModifyOrder>(),
        spec::<CancelOrder>(),
        spec::<CancelAllOrders>(),
        spec::<CountdownCancelAll>(),
    ]
}

pub(crate) fn _dict_unused(_: &Bound<'_, PyDict>) {}
