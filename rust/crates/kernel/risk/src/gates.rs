//! The two risk gates (docs/architecture.md sections 9.2 and 10.1). A rule reads one
//! [`OrderCheck`] (the order and a snapshot of what it would change) and the gate's shared state,
//! and returns the reason code that denies the order, or `None`. A gate is a fixed list of rules
//! checked in order; the first denial wins. The rate limit spends a token, so it runs last, after
//! every other rule passed.
//!
//! - Gate A (the intent): `TradingState`, instrument whitelist, instrument status, position
//!   notional including open orders;
//! - Gate B (the order): `TradingState`, price filter, lot size, minimum notional, maximum order
//!   notional, price band, open orders, reduce-only, margin, rate limit.
//!
//! Amounts in a limit are compared only in the limit's currency; a rule whose currency differs
//! from the instrument's notional currency does not apply.

use execution::OpenQuantity;
use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{FixedVec, Status, UnixNanos};
use model::data::InstrumentStatus;
use model::enums::{MarketStatusAction, OrderSide, OrderType, TradingState};
use model::fixed_point::{notional_raw, QUANTITY_RAW_MAX};
use model::instruments::Instrument;
use model::{Money, Price, Quantity};

use crate::checks::ReasonCode;
use crate::rate_limit::{RateLimiter, RateWindow};
use crate::trading_state::{allowed, CommandKind, TradingStateMachine, TradingTrigger};

pub const TRADING_HALTED: ReasonCode = "TRADING_HALTED";
pub const TRADING_REDUCING_ONLY: ReasonCode = "TRADING_REDUCING_ONLY";
pub const INSTRUMENT_NOT_ALLOWED: ReasonCode = "INSTRUMENT_NOT_ALLOWED";
pub const INSTRUMENT_NOT_TRADING: ReasonCode = "INSTRUMENT_NOT_TRADING";
pub const EXPOSURE_EXCEEDS_LIMIT: ReasonCode = "EXPOSURE_EXCEEDS_LIMIT";
pub const PRICE_INVALID_TICK: ReasonCode = "PRICE_INVALID_TICK";
pub const PRICE_OUT_OF_RANGE: ReasonCode = "PRICE_OUT_OF_RANGE";
pub const QUANTITY_INVALID_STEP: ReasonCode = "QUANTITY_INVALID_STEP";
pub const QUANTITY_OUT_OF_RANGE: ReasonCode = "QUANTITY_OUT_OF_RANGE";
pub const NOTIONAL_BELOW_MIN: ReasonCode = "NOTIONAL_BELOW_MIN";
pub const NOTIONAL_EXCEEDS_MAX: ReasonCode = "NOTIONAL_EXCEEDS_MAX_PER_ORDER";
pub const PRICE_OUTSIDE_BAND: ReasonCode = "PRICE_OUTSIDE_BAND";
pub const OPEN_ORDERS_EXCEEDED: ReasonCode = "OPEN_ORDERS_EXCEEDED";
pub const RATE_LIMIT_EXCEEDED: ReasonCode = "RATE_LIMIT_EXCEEDED";
pub const REDUCE_ONLY_INVALID: ReasonCode = "REDUCE_ONLY_INVALID";
pub const MARGIN_INSUFFICIENT: ReasonCode = "MARGIN_INSUFFICIENT";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RiskConfig {
    pub initial_state: TradingState,
    pub max_order_notional: Option<Money>,
    /// Per instrument, open orders included.
    pub max_position_notional: Option<Money>,
    /// Soft: `Reducing`.
    pub daily_loss_limit: Option<Money>,
    /// Hard: `Halted` and the kill switch.
    pub daily_loss_halt: Option<Money>,
    /// Soft.
    pub max_drawdown: Option<Money>,
    /// 0: off.
    pub price_band_bps: u32,
    /// Per instrument; 0: off.
    pub max_open_orders: u32,
    /// Binance USDⓈ-M allows 300; 0: off.
    pub orders_per_10s: u32,
    /// Binance USDⓈ-M allows 1200; 0: off.
    pub orders_per_minute: u32,
    /// Soft at 80% maintenance / equity; 0: off.
    pub margin_ratio_bps: u32,
    pub check_margin: bool,
    /// The venue-side dead man's switch (section 10.3); 0: off.
    pub countdown_cancel_ms: u32,
}

impl Default for RiskConfig {
    fn default() -> Self {
        Self {
            initial_state: TradingState::Active,
            max_order_notional: None,
            max_position_notional: None,
            daily_loss_limit: None,
            daily_loss_halt: None,
            max_drawdown: None,
            price_band_bps: 0,
            max_open_orders: 0,
            orders_per_10s: 250,
            orders_per_minute: 1000,
            margin_ratio_bps: 8000,
            check_margin: true,
            countdown_cancel_ms: 0,
        }
    }
}

/// Margin in the settlement currency (10^9 raw), when the account balance is known.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MarginCheck {
    /// Wallet + unrealized - initial margin of positions and orders.
    pub available_raw: i64,
    /// Initial margin of this order.
    pub required_raw: i64,
}

/// One order as the gates see it, with a snapshot of the state it would change. Built by the
/// kernel for every submit and modify.
#[derive(Clone, Copy, Debug)]
pub struct OrderCheck<'a> {
    pub instrument: &'a Instrument,
    pub slot: u32,
    pub strategy: u16,
    pub kind: CommandKind,
    pub side: OrderSide,
    pub order_type: OrderType,
    pub quantity: Quantity,
    pub price: Option<Price>,
    pub reduce_only: bool,
    pub now: UnixNanos,
    /// The venue position, signed.
    pub position_raw: i64,
    /// The instrument's open orders, before this one.
    pub open: OpenQuantity,
    /// Mark price, else last trade.
    pub reference: Option<Price>,
    pub margin: Option<MarginCheck>,
}

impl<'a> OrderCheck<'a> {
    /// A check with only the order filled in; the kernel adds the snapshot.
    #[must_use]
    pub fn new(
        instrument: &'a Instrument,
        kind: CommandKind,
        side: OrderSide,
        order_type: OrderType,
        quantity: Quantity,
        price: Option<Price>,
        now: UnixNanos,
    ) -> Self {
        Self {
            instrument,
            slot: 0,
            strategy: 0,
            kind,
            side,
            order_type,
            quantity,
            price,
            reduce_only: false,
            now,
            position_raw: 0,
            open: OpenQuantity::default(),
            reference: None,
            margin: None,
        }
    }

    /// The price the order's notional is measured at: its own, else the reference.
    fn order_price(&self) -> Option<Price> {
        self.price.or(self.reference)
    }
}

/// Read-only state the rules consult.
#[derive(Clone, Copy, Debug)]
pub struct GateState<'a> {
    pub config: &'a RiskConfig,
    pub trading_state: TradingState,
    pub instrument_allowed: bool,
    pub instrument_trading: bool,
}

pub type Rule = fn(&OrderCheck<'_>, &GateState<'_>) -> Option<ReasonCode>;

/// `|quantity| x price` in the quote currency, or `None` when it cannot be computed.
fn notional(
    instrument: &Instrument,
    quantity_raw: u64,
    precision: u8,
    price: Price,
) -> Option<Money> {
    let q = Quantity::from_raw(quantity_raw, precision).ok()?;
    let raw = notional_raw(price, q, instrument.multiplier).ok()?;
    Money::from_raw(raw, instrument.quote_currency).ok()
}

/// `|amount| > |limit|`, when both are in the same currency.
fn exceeds(amount: Option<Money>, limit: Option<Money>) -> bool {
    match (amount, limit) {
        (Some(a), Some(l)) => {
            a.currency() == l.currency() && a.raw().unsigned_abs() > l.raw().unsigned_abs()
        }
        _ => false,
    }
}

// ---- rules ------------------------------------------------------------------------------------

pub fn trading_state_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    if allowed(s.trading_state, c.kind) {
        return None;
    }
    Some(if s.trading_state == TradingState::Halted {
        TRADING_HALTED
    } else {
        TRADING_REDUCING_ONLY
    })
}

pub fn instrument_whitelist_rule(_: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    (!s.instrument_allowed).then_some(INSTRUMENT_NOT_ALLOWED)
}

/// A halted market still takes orders that only reduce: they may be the way out.
pub fn instrument_status_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    (!s.instrument_trading && c.kind != CommandKind::Reduce).then_some(INSTRUMENT_NOT_TRADING)
}

/// `|position + open orders on the order's side + the order|` at the reference price.
pub fn intent_notional_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    let limit = s.config.max_position_notional?;
    if c.kind != CommandKind::Open {
        return None;
    }
    let reference = c.reference?;
    let q = i128::from(c.quantity.raw());
    let worst = match c.side {
        OrderSide::Buy => i128::from(c.position_raw) + i128::from(c.open.buy_raw) + q,
        OrderSide::Sell => i128::from(c.position_raw) - i128::from(c.open.sell_raw) - q,
    };
    let magnitude = worst.unsigned_abs();
    if magnitude > u128::from(QUANTITY_RAW_MAX) {
        return Some(EXPOSURE_EXCEEDS_LIMIT);
    }
    let n = notional(c.instrument, magnitude as u64, c.quantity.precision(), reference);
    exceeds(n, Some(limit)).then_some(EXPOSURE_EXCEEDS_LIMIT)
}

/// `PRICE_FILTER`: the tick and the price range.
pub fn price_filter_rule(c: &OrderCheck<'_>, _: &GateState<'_>) -> Option<ReasonCode> {
    let price = c.price?;
    let i = c.instrument;
    let tick = i.price_increment.raw();
    if tick > 0 && price.raw() % tick != 0 {
        return Some(PRICE_INVALID_TICK);
    }
    if i.limits.min_price.is_some_and(|min| price < min)
        || i.limits.max_price.is_some_and(|max| price > max)
    {
        return Some(PRICE_OUT_OF_RANGE);
    }
    None
}

/// `LOT_SIZE`: the step and the quantity range.
pub fn lot_size_rule(c: &OrderCheck<'_>, _: &GateState<'_>) -> Option<ReasonCode> {
    let i = c.instrument;
    let step = i.size_increment.raw();
    if step > 0 && c.quantity.raw() % step != 0 {
        return Some(QUANTITY_INVALID_STEP);
    }
    if i.limits.min_quantity.is_some_and(|min| c.quantity.raw() < min.raw())
        || i.limits.max_quantity.is_some_and(|max| c.quantity.raw() > max.raw())
    {
        return Some(QUANTITY_OUT_OF_RANGE);
    }
    None
}

/// `MIN_NOTIONAL`; Binance exempts reduce-only orders.
pub fn min_notional_rule(c: &OrderCheck<'_>, _: &GateState<'_>) -> Option<ReasonCode> {
    let min = c.instrument.limits.min_notional?;
    let px = c.order_price()?;
    if c.reduce_only || !c.kind.is_new_order() {
        return None;
    }
    let n = notional(c.instrument, c.quantity.raw(), c.quantity.precision(), px)?;
    (n.currency() == min.currency() && n.raw() < min.raw()).then_some(NOTIONAL_BELOW_MIN)
}

/// `[risk] max_order_notional` and the instrument's `max_notional`.
pub fn max_order_notional_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    let px = c.order_price()?;
    let n = notional(c.instrument, c.quantity.raw(), c.quantity.precision(), px);
    (exceeds(n, s.config.max_order_notional) || exceeds(n, c.instrument.limits.max_notional))
        .then_some(NOTIONAL_EXCEEDS_MAX)
}

/// A limit price further than `price_band_bps` from the reference.
pub fn price_band_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    let band = s.config.price_band_bps;
    let (price, reference) = (c.price?, c.reference?);
    if band == 0 || reference.raw() <= 0 {
        return None;
    }
    let distance = (i128::from(price.raw()) - i128::from(reference.raw())).unsigned_abs();
    (distance * 10_000 > u128::from(reference.raw().unsigned_abs()) * u128::from(band))
        .then_some(PRICE_OUTSIDE_BAND)
}

/// `MAX_NUM_ORDERS` and `[risk] max_open_orders`, whichever is smaller.
pub fn max_open_orders_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    let limit = s.config.max_open_orders;
    if limit == 0 || !c.kind.is_new_order() {
        return None;
    }
    (c.open.orders >= limit).then_some(OPEN_ORDERS_EXCEEDED)
}

/// One-way mode: a reduce-only order must reduce the venue position (the kernel classifies it
/// as `Reduce` when its side opposes the position and its quantity fits in it).
pub fn position_mode_rule(c: &OrderCheck<'_>, _: &GateState<'_>) -> Option<ReasonCode> {
    (c.reduce_only && c.kind.is_new_order() && c.kind != CommandKind::Reduce)
        .then_some(REDUCE_ONLY_INVALID)
}

/// The command kind of a new order of `side` for `quantity_raw` against a signed position:
/// `Reduce` when it opposes the position and fits in it, else `Open`.
#[must_use]
pub const fn classify(side: OrderSide, quantity_raw: u64, position_raw: i64) -> CommandKind {
    let opposite = match side {
        OrderSide::Buy => position_raw < 0,
        OrderSide::Sell => position_raw > 0,
    };
    if opposite && quantity_raw <= position_raw.unsigned_abs() {
        CommandKind::Reduce
    } else {
        CommandKind::Open
    }
}

/// The order's initial margin must fit in what the account has left. Orders that only reduce
/// free margin and always pass.
pub fn margin_rule(c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    let m = c.margin?;
    if !s.config.check_margin || c.kind != CommandKind::Open {
        return None;
    }
    (m.required_raw > m.available_raw).then_some(MARGIN_INSUFFICIENT)
}

pub const GATE_A: &[Rule] =
    &[trading_state_rule, instrument_whitelist_rule, instrument_status_rule, intent_notional_rule];
pub const GATE_B: &[Rule] = &[
    trading_state_rule,
    price_filter_rule,
    lot_size_rule,
    min_notional_rule,
    max_order_notional_rule,
    price_band_rule,
    max_open_orders_rule,
    position_mode_rule,
    margin_rule,
];
/// A modify changes price and quantity of an order that already passed both gates.
pub const MODIFY_GATE: &[Rule] = &[
    trading_state_rule,
    price_filter_rule,
    lot_size_rule,
    max_order_notional_rule,
    price_band_rule,
];

#[must_use]
pub fn run_gate(gate: &[Rule], c: &OrderCheck<'_>, s: &GateState<'_>) -> Option<ReasonCode> {
    gate.iter().find_map(|rule| rule(c, s))
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RiskStats {
    pub checked: u64,
    pub denied_a: u64,
    pub denied_b: u64,
    pub modify_rejected: u64,
    pub kill_switches: u64,
    pub state_changes: u64,
}
kernel_core::state_fields!(RiskStats {
    checked,
    denied_a,
    denied_b,
    modify_rejected,
    kill_switches,
    state_changes
});

/// The gates' state: configuration, `TradingState`, rate limit, instrument statuses and
/// whitelists. The post-trade monitors arrive with a later R3 step.
#[derive(Clone, Debug)]
pub struct RiskEngine {
    config: RiskConfig,
    state: TradingStateMachine,
    limiter: RateLimiter,
    instruments: u32,
    /// By slot: whether the market is trading.
    trading: FixedVec<bool>,
    /// Strategy x slot.
    allowed: FixedVec<bool>,
    /// By strategy: whether `allowed` applies.
    restricted: FixedVec<bool>,
    stats: RiskStats,
}

impl RiskEngine {
    #[must_use]
    pub fn new(config: RiskConfig, instruments: u32, strategies: u16) -> Self {
        let windows = [
            RateWindow {
                interval_ns: if config.orders_per_10s > 0 { 10_000_000_000 } else { 0 },
                limit: config.orders_per_10s,
            },
            RateWindow {
                interval_ns: if config.orders_per_minute > 0 { 60_000_000_000 } else { 0 },
                limit: config.orders_per_minute,
            },
        ];
        Self {
            config,
            state: TradingStateMachine::new(config.initial_state),
            limiter: RateLimiter::new(&windows),
            instruments,
            trading: FixedVec::from_iter_exact((0..instruments).map(|_| true)),
            allowed: FixedVec::from_iter_exact(
                (0..instruments as usize * strategies as usize).map(|_| false),
            ),
            restricted: FixedVec::from_iter_exact((0..strategies).map(|_| false)),
            stats: RiskStats::default(),
        }
    }

    #[must_use]
    pub const fn config(&self) -> &RiskConfig {
        &self.config
    }
    #[must_use]
    pub const fn trading_state(&self) -> TradingState {
        self.state.state()
    }
    #[must_use]
    pub const fn state_machine(&self) -> &TradingStateMachine {
        &self.state
    }
    #[must_use]
    pub const fn stats(&self) -> &RiskStats {
        &self.stats
    }
    pub fn limiter(&mut self) -> &mut RateLimiter {
        &mut self.limiter
    }

    /// Returns whether the effective state changed.
    pub fn apply(&mut self, trigger: TradingTrigger) -> bool {
        let changed = self.state.apply(trigger);
        self.stats.state_changes += u64::from(changed);
        changed
    }

    pub fn note_kill_switch(&mut self) {
        self.stats.kill_switches += 1;
    }

    /// Restricts strategy `s` to the instruments later allowed with [`Self::allow`].
    pub fn restrict(&mut self, s: u16) {
        if let Some(r) = self.restricted.get_mut(s as usize) {
            *r = true;
        }
    }
    pub fn allow(&mut self, s: u16, slot: u32) {
        if (s as usize) < self.restricted.len() && slot < self.instruments {
            self.allowed[s as usize * self.instruments as usize + slot as usize] = true;
        }
    }

    pub fn on_status(&mut self, slot: u32, status: &InstrumentStatus) {
        let Some(t) = self.trading.get_mut(slot as usize) else { return };
        let mut trading = !matches!(
            status.action,
            MarketStatusAction::Halt
                | MarketStatusAction::Pause
                | MarketStatusAction::Suspend
                | MarketStatusAction::Close
                | MarketStatusAction::PostClose
                | MarketStatusAction::NotAvailableForTrading
        );
        if let Some(is_trading) = status.is_trading {
            trading = is_trading;
        }
        *t = trading;
    }

    #[must_use]
    pub fn instrument_trading(&self, slot: u32) -> bool {
        self.trading.get(slot as usize).copied().unwrap_or(true)
    }

    /// Gate A then Gate B, then the rate limit; `None` when the order may go.
    pub fn check_order(&mut self, c: &OrderCheck<'_>) -> Option<ReasonCode> {
        self.stats.checked += 1;
        let s = self.gate_state(c);
        if let Some(denied) = run_gate(GATE_A, c, &s) {
            self.stats.denied_a += 1;
            return Some(denied);
        }
        let denied = run_gate(GATE_B, c, &s)
            .or_else(|| (!self.limiter.try_acquire(c.now, 1)).then_some(RATE_LIMIT_EXCEEDED));
        self.stats.denied_b += u64::from(denied.is_some());
        denied
    }

    /// A parent order of an execution algorithm: Gate A only.
    pub fn check_parent(&mut self, c: &OrderCheck<'_>) -> Option<ReasonCode> {
        self.stats.checked += 1;
        let denied = run_gate(GATE_A, c, &self.gate_state(c));
        self.stats.denied_a += u64::from(denied.is_some());
        denied
    }

    /// A child order of an execution algorithm (its parent passed Gate A): Gate B, then the rate
    /// limit.
    pub fn check_child(&mut self, c: &OrderCheck<'_>) -> Option<ReasonCode> {
        self.stats.checked += 1;
        let denied = run_gate(GATE_B, c, &self.gate_state(c))
            .or_else(|| (!self.limiter.try_acquire(c.now, 1)).then_some(RATE_LIMIT_EXCEEDED));
        self.stats.denied_b += u64::from(denied.is_some());
        denied
    }

    pub fn check_modify(&mut self, c: &OrderCheck<'_>) -> Option<ReasonCode> {
        self.stats.checked += 1;
        let denied = run_gate(MODIFY_GATE, c, &self.gate_state(c))
            .or_else(|| (!self.limiter.try_acquire(c.now, 1)).then_some(RATE_LIMIT_EXCEEDED));
        self.stats.modify_rejected += u64::from(denied.is_some());
        denied
    }

    fn gate_state(&self, c: &OrderCheck<'_>) -> GateState<'_> {
        let restricted = self.restricted.get(c.strategy as usize).copied().unwrap_or(false);
        GateState {
            config: &self.config,
            trading_state: self.state.state(),
            instrument_allowed: !restricted
                || (c.slot < self.instruments
                    && self.allowed
                        [c.strategy as usize * self.instruments as usize + c.slot as usize]),
            instrument_trading: self.instrument_trading(c.slot),
        }
    }
}

/// The configuration is not part of the snapshot.
impl State for RiskEngine {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.state.write(w);
        self.limiter.write(w);
        self.trading.write(w);
        self.allowed.write(w);
        self.restricted.write(w);
        self.stats.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let (t, a, s) = (self.trading.len(), self.allowed.len(), self.restricted.len());
        self.state.read(r);
        self.limiter.read(r);
        self.trading.read(r);
        self.allowed.read(r);
        self.restricted.read(r);
        self.stats.read(r);
        if self.trading.len() != t || self.allowed.len() != a || self.restricted.len() != s {
            r.fail(Status::InvalidState);
        }
    }
}
