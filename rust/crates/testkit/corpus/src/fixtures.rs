//! Hand-built values the layer tests share: one perpetual, an account, and order events of every
//! kind on one order. Test support, like the corpus itself.

use kernel_core::UnixNanos;
use model::account::{AccountBalance, AccountState};
use model::enums::{AccountType, AssetClass, LiquiditySide, OrderSide, OrderType};
use model::identifiers::{
    AccountId, ClientOrderId, InstrumentId, StrategyId, Symbol, TradeId, TraderId, VenueOrderId,
};
use model::instruments::{Instrument, InstrumentKind, InstrumentLimits};
use model::order_events::{
    FillInfoFlags, OrderAccepted, OrderCancelRejected, OrderCanceled, OrderDenied, OrderEmulated,
    OrderEvent, OrderEventHeader, OrderExpired, OrderFillVoided, OrderFilled, OrderInitialized,
    OrderModifyRejected, OrderPendingCancel, OrderPendingUpdate, OrderRejected, OrderReleased,
    OrderSubmitted, OrderTriggered, OrderUpdated, Reason,
};
use model::{Currency, Money, Price, Quantity, Uuid4, FIXED_SCALAR};

#[must_use]
pub fn usdt() -> Currency {
    Currency::builtin_by_code("USDT").expect("USDT")
}
#[must_use]
pub fn btc() -> Currency {
    Currency::builtin_by_code("BTC").expect("BTC")
}
#[must_use]
pub fn instrument_id() -> InstrumentId {
    InstrumentId::parse("BTCUSDT-PERP.BINANCE").expect("instrument id")
}
#[must_use]
pub fn account_id() -> AccountId {
    AccountId::new("SIM-001").expect("account id")
}
#[must_use]
pub fn trader_id() -> TraderId {
    TraderId::new("JARVIS-001").expect("trader id")
}
#[must_use]
pub fn strategy_id() -> StrategyId {
    StrategyId::new("strategy-001").expect("strategy id")
}

/// `BTCUSDT-PERP.BINANCE`: tick 0.1, lot `10^-size_precision`, multiplier 1, margins 5% / 2.5%.
#[must_use]
pub fn perpetual(size_precision: u8) -> Instrument {
    Instrument::new(
        instrument_id(),
        Symbol::new("BTCUSDT").expect("symbol"),
        AssetClass::Cryptocurrency,
        InstrumentKind::CryptoPerpetual { settlement_currency: usdt(), is_inverse: false },
        Some(btc()),
        usdt(),
        Price::from_units(1, 1).expect("tick"),
        Quantity::from_units(1, size_precision).expect("lot"),
        Quantity::from_units(1, 0).expect("multiplier"),
        None,
        InstrumentLimits::default(),
        Quantity::parse("0.05").expect("margin"),
        Quantity::parse("0.025").expect("margin"),
        UnixNanos::new(1),
        UnixNanos::new(1),
    )
    .expect("perpetual")
}

/// An account holding `usdt_units` USDT, all free.
#[must_use]
pub fn account(usdt_units: i64, ts: UnixNanos) -> AccountState {
    let total = Money::from_raw(usdt_units * FIXED_SCALAR, usdt()).expect("money");
    let mut balances = kernel_core::FixedVec::with_capacity(1);
    let _ = balances.push(AccountBalance::new(total, Money::zero(usdt()), total).expect("balance"));
    AccountState::new(
        account_id(),
        AccountType::Margin,
        None,
        balances,
        kernel_core::FixedVec::with_capacity(0),
        true,
        Uuid4::from_u64s(1, 1),
        ts,
        ts,
    )
    .expect("account state")
}

#[must_use]
pub fn header(client_order_id: ClientOrderId, ts: UnixNanos) -> OrderEventHeader {
    OrderEventHeader {
        trader_id: trader_id(),
        strategy_id: strategy_id(),
        instrument_id: instrument_id(),
        client_order_id,
        event_id: Uuid4::from_u64s(ts.value(), 7),
        ts_event: ts,
        ts_init: ts,
        causation_id: None,
    }
}

/// What the quantity-carrying events need.
#[derive(Clone, Copy, Debug)]
pub struct Fill {
    pub trade_id: TradeId,
    pub quantity: Quantity,
    pub price: Price,
    pub side: OrderSide,
    pub liquidity: LiquiditySide,
    pub commission: Option<Money>,
    pub flags: FillInfoFlags,
}

impl Fill {
    #[must_use]
    pub fn new(trade_id: &str, quantity: Quantity, price: Price) -> Self {
        Self {
            trade_id: TradeId::new(trade_id).expect("trade id"),
            quantity,
            price,
            side: OrderSide::Buy,
            liquidity: LiquiditySide::Maker,
            commission: None,
            flags: FillInfoFlags::default(),
        }
    }
}

/// The order event with wire `tag` (1 `Initialized` .. 17 `FillVoided`) on `header`; `fill`
/// supplies the quantities of `Updated`, `Filled` and `FillVoided`.
#[must_use]
#[allow(clippy::too_many_lines)] // one arm per variant
pub fn order_event(tag: u8, header: OrderEventHeader, fill: &Fill) -> OrderEvent {
    let reason = Reason::from_static("TEST");
    let venue = VenueOrderId::new("V-1").expect("venue order id");
    match tag {
        1 => OrderEvent::Initialized(OrderInitialized {
            header,
            order_side: fill.side,
            order_type: OrderType::Limit,
            quantity: fill.quantity,
            time_in_force: model::enums::TimeInForce::Gtc,
            post_only: false,
            reduce_only: false,
            quote_quantity: false,
            reconciliation: false,
            price: Some(fill.price),
            activation_price: None,
            trigger_price: None,
            trigger_type: None,
            limit_offset: None,
            trailing_offset: None,
            trailing_offset_type: None,
            expire_time: None,
            display_qty: None,
            emulation_trigger: None,
            trigger_instrument_id: None,
            contingency_type: None,
            order_list_id: None,
            parent_order_id: None,
            exec_algorithm_id: None,
            exec_spawn_id: None,
        }),
        2 => OrderEvent::Denied(OrderDenied { header, reason }),
        3 => OrderEvent::Emulated(OrderEmulated { header }),
        4 => OrderEvent::Released(OrderReleased { header, released_price: fill.price }),
        5 => OrderEvent::Submitted(OrderSubmitted { header, account_id: account_id() }),
        6 => OrderEvent::Accepted(OrderAccepted {
            header,
            venue_order_id: venue,
            account_id: account_id(),
            reconciliation: false,
        }),
        7 => OrderEvent::Rejected(OrderRejected {
            header,
            account_id: account_id(),
            reason,
            reconciliation: false,
            due_post_only: false,
        }),
        8 => OrderEvent::Canceled(OrderCanceled {
            header,
            venue_order_id: Some(venue),
            account_id: Some(account_id()),
            reason: None,
            reconciliation: false,
        }),
        9 => OrderEvent::Expired(OrderExpired {
            header,
            venue_order_id: Some(venue),
            account_id: Some(account_id()),
            reconciliation: false,
        }),
        10 => OrderEvent::Triggered(OrderTriggered {
            header,
            venue_order_id: Some(venue),
            account_id: Some(account_id()),
            reconciliation: false,
        }),
        11 => OrderEvent::PendingUpdate(OrderPendingUpdate {
            header,
            account_id: account_id(),
            venue_order_id: Some(venue),
            reconciliation: false,
        }),
        12 => OrderEvent::PendingCancel(OrderPendingCancel {
            header,
            account_id: account_id(),
            venue_order_id: Some(venue),
            reconciliation: false,
        }),
        13 => OrderEvent::ModifyRejected(OrderModifyRejected {
            header,
            reason,
            venue_order_id: Some(venue),
            account_id: Some(account_id()),
            reconciliation: false,
        }),
        14 => OrderEvent::CancelRejected(OrderCancelRejected {
            header,
            reason,
            venue_order_id: Some(venue),
            account_id: Some(account_id()),
            reconciliation: false,
        }),
        15 => OrderEvent::Updated(OrderUpdated {
            header,
            venue_order_id: Some(venue),
            account_id: Some(account_id()),
            quantity: fill.quantity,
            price: Some(fill.price),
            trigger_price: None,
            protection_price: None,
            is_quote_quantity: false,
            reconciliation: false,
        }),
        16 => OrderEvent::Filled(OrderFilled {
            header,
            venue_order_id: venue,
            account_id: account_id(),
            trade_id: fill.trade_id,
            order_side: fill.side,
            order_type: OrderType::Limit,
            last_qty: fill.quantity,
            last_px: fill.price,
            currency: usdt(),
            liquidity_side: fill.liquidity,
            reconciliation: false,
            position_id: None,
            commission: fill.commission,
            info_flags: fill.flags,
        }),
        17 => OrderEvent::FillVoided(OrderFillVoided {
            header,
            venue_order_id: venue,
            account_id: account_id(),
            correction_id: TradeId::new("C-1").expect("trade id"),
            trade_id: fill.trade_id,
            voided_qty: fill.quantity,
            commission_voided: None,
            order_side: fill.side,
            order_type: OrderType::Limit,
            last_px: fill.price,
            currency: usdt(),
            liquidity_side: fill.liquidity,
            position_id: None,
            reason: None,
            reconciliation: false,
            is_reopened: false,
            info_flags: FillInfoFlags::default(),
        }),
        _ => panic!("no order event with tag {tag}"),
    }
}
