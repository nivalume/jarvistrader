//! The simulated venue (docs/architecture.md sections 12.1 and 12.3). It sees market data and
//! commands at venue time and answers with the order events Binance would send: accepted,
//! rejected, filled, canceled, expired, updated, and the rejections of modifies and cancels.
//!
//! Market state per instrument: the top of book from quotes, and an L2 book once deltas arrive
//! (L2 then takes precedence). Our orders are not part of that book; they match against it.
//!
//! - taker: an order that crosses on arrival (or after a modify) takes the opposite side, best
//!   level first: L2 levels up to its limit, or the top of book capped by its size. `IOC` and
//!   market remainders expire, `FOK` expires unless it fills completely, `GTC` and `GTD`
//!   remainders rest. A post-only (GTX) order that would take is rejected (-5022).
//! - maker: a resting order fills at its own price. `TopOfBook`: when the opposite best reaches
//!   its price (up to that size), or a trade prints through it (up to the trade's size).
//!   `QueuePosition`: as `TopOfBook`, and also at its own price once the volume ahead of it in
//!   the queue has traded: trades at the price consume the queue ahead, size decreases at the
//!   price shrink it in proportion, size increases join behind it. `specs/tla/Matching.tla` owns
//!   this model; `tests/backtest.rs` replays its behaviours.
//! - STP: an arriving order that would cross one of our resting orders on the other side
//!   expires (`ExpireTaker`), expires those resting orders (`ExpireMaker`), or both.
//! - fees from the fee model, in the settlement currency, on every fill.
//! - GTD orders expire at the first venue event at or after their expire time.
//!
//! The venue keeps its own account (positions and balances, a `Portfolio`) for reduce-only checks
//! and snapshots.

use cost::MakerTakerFees;
use data::book::{BookConfig, BookLevel, OrderBook, Side};
use data::intern::InstrumentSlot;
use data::subscription::StrategyIndex;
use kernel_core::rng::CounterRng;
use kernel_core::{FixedVec, Result, Status, UnixNanos};
use model::account::AccountState;
use model::data::{OrderBookDeltas, QuoteTick, TradeTick};
use model::enums::{AggressorSide, BookAction, LiquiditySide, OrderSide, OrderType, TimeInForce};
use model::instruments::Instrument;
use model::order_events::{
    OrderAccepted, OrderCancelRejected, OrderCanceled, OrderEvent, OrderEventHeader, OrderExpired,
    OrderFilled, OrderModifyRejected, OrderRejected, OrderUpdated, Reason,
};
use model::outputs::{CancelOrder, ModifyOrder, Output, SubmitOrder};
use model::{
    AccountId, ClientOrderId, Event, InstrumentId, Price, Quantity, StrategyId, TradeId, TraderId,
    Uuid4, VenueOrderId, FIXED_PRECISION,
};
use portfolio::{MarginModel, Portfolio, PortfolioConfig};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FillModel {
    #[default]
    TopOfBook,
    QueuePosition,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum StpMode {
    #[default]
    None,
    ExpireTaker,
    ExpireMaker,
    ExpireBoth,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SimConfig {
    pub fill_model: FillModel,
    pub stp: StpMode,
    pub fees: MakerTakerFees,
    pub instruments: u32,
    pub strategies: u16,
    /// Resting orders.
    pub orders: u32,
    /// L2 window per instrument, in ticks.
    pub book_levels: u32,
    pub book_overflow_levels: u32,
    /// Levels one taker order may consume.
    pub walk_levels: u32,
    /// Venue events per call.
    pub events: u32,
    pub margin: MarginModel,
    pub account_id: AccountId,
    pub trader_id: TraderId,
    pub seed: u64,
}

impl Default for SimConfig {
    fn default() -> Self {
        Self {
            fill_model: FillModel::TopOfBook,
            stp: StpMode::None,
            fees: MakerTakerFees::schedule("zero").expect("zero fees"),
            instruments: 64,
            strategies: 8,
            orders: 4096,
            book_levels: 4096,
            book_overflow_levels: 1024,
            walk_levels: 64,
            events: 4096,
            margin: MarginModel::Standard,
            account_id: AccountId::new("SIM-001").unwrap_or_default(),
            trader_id: TraderId::new("JARVIS-001").unwrap_or_default(),
            seed: 0,
        }
    }
}

pub mod reject {
    pub const UNKNOWN_SYMBOL: &str = "-1121 INVALID_SYMBOL";
    pub const POST_ONLY: &str = "-5022 POST_ONLY_WOULD_TAKE";
    pub const REDUCE_ONLY: &str = "-2022 REDUCE_ONLY_REJECTED";
    pub const NO_MARKET: &str = "NO_MARKET";
    pub const CAPACITY: &str = "VENUE_CAPACITY";
    pub const UNKNOWN_ORDER: &str = "-2011 UNKNOWN_ORDER";
    pub const ORDER_MISSING: &str = "-2013 ORDER_DOES_NOT_EXIST";
    pub const QUANTITY_BELOW_FILLED: &str = "-4028 QUANTITY_BELOW_FILLED";
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SimStats {
    pub commands: u64,
    pub fills: u64,
    pub maker_fills: u64,
    pub taker_fills: u64,
    pub rejected: u64,
    pub expired: u64,
    pub canceled: u64,
}

/// A resting order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SimOrder {
    pub client_order_id: ClientOrderId,
    pub venue_order_id: VenueOrderId,
    pub strategy: u16,
    pub slot: u32,
    pub instrument_id: InstrumentId,
    pub side: OrderSide,
    pub order_type: OrderType,
    pub time_in_force: TimeInForce,
    pub post_only: bool,
    pub reduce_only: bool,
    pub price: Price,
    pub quantity: Quantity,
    pub filled_raw: u64,
    pub expire_time: Option<UnixNanos>,
    /// Volume ahead in the queue (`QueuePosition`).
    pub ahead_raw: u64,
}

impl SimOrder {
    #[must_use]
    pub const fn leaves_raw(&self) -> u64 {
        self.quantity.raw() - self.filled_raw
    }
}

#[derive(Clone, Debug)]
struct Market {
    instrument: Option<Instrument>,
    bid: Option<Price>,
    ask: Option<Price>,
    bid_size: u64,
    ask_size: u64,
    /// L2, from deltas.
    book: Option<OrderBook>,
}

impl Market {
    const fn empty() -> Self {
        Self { instrument: None, bid: None, ask: None, bid_size: 0, ask_size: 0, book: None }
    }
}

#[derive(Clone, Debug)]
pub struct SimulatedExchange {
    config: SimConfig,
    markets: FixedVec<Market>,
    orders: FixedVec<Option<SimOrder>>,
    strategy_ids: FixedVec<StrategyId>,
    levels: FixedVec<BookLevel>,
    out: FixedVec<OrderEvent>,
    portfolio: Portfolio,
    rng: CounterRng,
    stats: SimStats,
    now: UnixNanos,
    event_serial: u64,
    venue_serial: u64,
    trade_serial: u64,
}

/// Venue event ids draw from their own key.
const SALT: u64 = 0x3C6E_F372_FE94_F82B;

fn within(side: OrderSide, level: Price, limit: Option<Price>) -> bool {
    match limit {
        None => true,
        Some(limit) => match side {
            OrderSide::Buy => level <= limit,
            OrderSide::Sell => level >= limit,
        },
    }
}

impl SimulatedExchange {
    #[must_use]
    pub fn new(c: SimConfig) -> Self {
        Self {
            config: c,
            markets: FixedVec::from_iter_exact((0..c.instruments).map(|_| Market::empty())),
            orders: FixedVec::from_iter_exact((0..c.orders).map(|_| None)),
            strategy_ids: FixedVec::from_iter_exact(
                (0..c.strategies).map(strategy::trading::default_strategy_id),
            ),
            levels: FixedVec::from_iter_exact((0..c.walk_levels).map(|_| BookLevel::default())),
            out: FixedVec::with_capacity(c.events as usize),
            portfolio: Portfolio::new(PortfolioConfig {
                instruments: c.instruments,
                strategies: 1,
                margin: c.margin,
                currencies: 16,
            }),
            rng: CounterRng::new(c.seed ^ SALT),
            stats: SimStats::default(),
            now: UnixNanos::default(),
            event_serial: 0,
            venue_serial: 0,
            trade_serial: 0,
        }
    }

    pub fn set_strategy_id(&mut self, s: u16, id: StrategyId) {
        if let Some(slot) = self.strategy_ids.get_mut(s as usize) {
            *slot = id;
        }
    }

    /// The events the last call produced, at venue time (`ts_event = ts_init` = venue time).
    #[must_use]
    pub fn events(&self) -> &[OrderEvent] {
        &self.out
    }
    pub fn clear_events(&mut self) {
        self.out.clear();
    }
    #[must_use]
    pub const fn stats(&self) -> &SimStats {
        &self.stats
    }
    #[must_use]
    pub const fn account(&self) -> &Portfolio {
        &self.portfolio
    }
    pub fn set_account(&mut self, state: &AccountState) -> Result<()> {
        self.portfolio.set_account(state)
    }
    /// Resting orders, oldest slot first.
    pub fn open_orders(&self) -> impl Iterator<Item = &SimOrder> {
        self.orders.iter().flatten()
    }

    // ---- market data (venue time) -------------------------------------------------------------

    pub fn on_data(&mut self, event: &Event, now: UnixNanos) -> Result<()> {
        self.now = now;
        match event {
            Event::QuoteTick(q) => self.on_quote(q)?,
            Event::TradeTick(t) => self.on_trade(t)?,
            Event::OrderBookDeltas(d) => self.on_deltas(d)?,
            Event::MarkPriceUpdate(m) => {
                if let Some(slot) = self.slot_of(&m.instrument_id) {
                    self.portfolio.on_mark(InstrumentSlot(slot), m.value)?;
                }
            }
            Event::FundingRateUpdate(f) => {
                if let Some(slot) = self.slot_of(&f.instrument_id) {
                    if let Some(def) = self.markets[slot as usize].instrument {
                        self.portfolio.on_funding(&def, InstrumentSlot(slot), f)?;
                    }
                }
            }
            Event::Instrument(i) => self.define(i)?,
            Event::AccountState(a) => self.portfolio.set_account(a)?,
            _ => {}
        }
        self.expire_due()
    }

    // ---- commands (venue time) ----------------------------------------------------------------

    pub fn on_command(&mut self, command: &Output, now: UnixNanos) -> Result<()> {
        self.now = now;
        match command {
            Output::SubmitOrder(c) => {
                self.stats.commands += 1;
                self.on_submit(c)?;
            }
            Output::ModifyOrder(c) => {
                self.stats.commands += 1;
                self.on_modify(c)?;
            }
            Output::CancelOrder(c) => {
                self.stats.commands += 1;
                self.on_cancel(c)?;
            }
            Output::CancelAllOrders(c) => {
                self.stats.commands += 1;
                for i in 0..self.orders.len() {
                    if self.orders[i].is_some_and(|o| o.instrument_id == c.instrument_id) {
                        self.cancel(i)?;
                    }
                }
            }
            _ => {}
        }
        self.expire_due()
    }

    // ---- data handlers ------------------------------------------------------------------------

    fn define(&mut self, instrument: &Instrument) -> Result<()> {
        let slot = match self.slot_of(&instrument.id) {
            Some(s) => s,
            None => self
                .markets
                .iter()
                .position(|m| m.instrument.is_none())
                .ok_or(Status::CapacityExceeded)? as u32,
        };
        self.markets[slot as usize].instrument = Some(*instrument);
        Ok(())
    }

    fn on_quote(&mut self, q: &QuoteTick) -> Result<()> {
        let Some(slot) = self.slot_of(&q.instrument_id) else { return Ok(()) };
        let m = &mut self.markets[slot as usize];
        let before = (m.bid, m.ask, m.bid_size, m.ask_size);
        m.bid = Some(q.bid_price);
        m.ask = Some(q.ask_price);
        m.bid_size = q.bid_size.raw();
        m.ask_size = q.ask_size.raw();
        if m.book.is_some() {
            return Ok(()); // the L2 book drives matching
        }
        if self.config.fill_model == FillModel::QueuePosition {
            let after = (m.bid, m.ask, m.bid_size, m.ask_size);
            for o in self.orders.iter_mut().flatten().filter(|o| o.slot == slot) {
                Self::follow_top(o, before, after);
            }
        }
        self.match_resting(slot)
    }

    /// `QueuePosition` on quotes: the order's level shrank (in proportion), or the market left it.
    fn follow_top(
        o: &mut SimOrder,
        before: (Option<Price>, Option<Price>, u64, u64),
        after: (Option<Price>, Option<Price>, u64, u64),
    ) {
        let buy = o.side == OrderSide::Buy;
        let (was, same) = if buy { (before.0, after.0) } else { (before.1, after.1) };
        if same == Some(o.price) && was == Some(o.price) {
            let (b, a) = if buy { (before.2, after.2) } else { (before.3, after.3) };
            Self::shrink_queue(o, b, a);
            return;
        }
        let left = match same {
            None => true,
            Some(p) => {
                if buy {
                    p < o.price
                } else {
                    p > o.price
                }
            }
        };
        if left {
            o.ahead_raw = 0; // nothing is ahead of the order at its level any more
        }
    }

    fn on_deltas(&mut self, d: &OrderBookDeltas) -> Result<()> {
        let Some(slot) = self.slot_of(&d.instrument_id) else { return Ok(()) };
        if d.deltas.is_empty() {
            return Ok(());
        }
        let m = &mut self.markets[slot as usize];
        if m.book.is_none() {
            let def = m.instrument.ok_or(Status::InvalidState)?;
            m.book = Some(OrderBook::new(BookConfig {
                price_increment: def.price_increment,
                size_precision: def.size_precision,
                window_levels: self.config.book_levels,
                overflow_levels: self.config.book_overflow_levels,
            })?);
        }
        for delta in &d.deltas {
            let book = self.markets[slot as usize].book.as_mut().ok_or(Status::InvalidState)?;
            if self.config.fill_model == FillModel::QueuePosition
                && delta.action != BookAction::Clear
            {
                if let Some(side) = delta.order.side {
                    let before = book.size_at(Side::from(side), delta.order.price).raw();
                    book.apply_delta(delta)?;
                    let after = book.size_at(Side::from(side), delta.order.price).raw();
                    for o in self.orders.iter_mut().flatten() {
                        if o.slot == slot && o.side == side && o.price == delta.order.price {
                            Self::shrink_queue(o, before, after);
                        }
                    }
                    continue;
                }
            }
            book.apply_delta(delta)?;
        }
        self.match_resting(slot)
    }

    fn on_trade(&mut self, t: &TradeTick) -> Result<()> {
        let Some(slot) = self.slot_of(&t.instrument_id) else { return Ok(()) };
        for i in 0..self.orders.len() {
            let Some(o) = self.orders[i].as_mut() else { continue };
            if o.slot != slot {
                continue;
            }
            let volume = Self::trade_volume(o, t, self.config.fill_model);
            if volume > 0 {
                let qty = volume.min(o.leaves_raw());
                let price = o.price;
                self.fill(i, qty, price, LiquiditySide::Maker)?;
            }
        }
        Ok(())
    }

    /// What a trade gives a resting order: all of it when it prints through the order's price,
    /// and at the price (`QueuePosition`) what is left after the queue ahead.
    fn trade_volume(o: &mut SimOrder, t: &TradeTick, model: FillModel) -> u64 {
        let buy = o.side == OrderSide::Buy;
        // A resting buy is hit by sellers, a resting sell lifted by buyers.
        let hits = if buy {
            t.aggressor_side != AggressorSide::Buy
        } else {
            t.aggressor_side != AggressorSide::Sell
        };
        if !hits {
            return 0;
        }
        if if buy { t.price < o.price } else { t.price > o.price } {
            return t.size.raw();
        }
        if t.price != o.price || model != FillModel::QueuePosition {
            return 0;
        }
        let size = t.size.raw();
        if size <= o.ahead_raw {
            o.ahead_raw -= size;
            return 0;
        }
        let volume = size - o.ahead_raw;
        o.ahead_raw = 0;
        volume
    }

    /// Volume ahead shrinks with the level: cancellations are spread over the queue in
    /// proportion, rounded down to the order's lot so that every fill quantity stays on the lot
    /// grid.
    fn shrink_queue(o: &mut SimOrder, before: u64, after: u64) {
        if after >= before || before == 0 {
            return; // growth joins behind the order
        }
        let lot = kernel_core::int_math::POW10[(FIXED_PRECISION - o.quantity.precision()) as usize];
        let shrunk = (u128::from(o.ahead_raw) * u128::from(after) / u128::from(before)) as u64;
        o.ahead_raw = shrunk - shrunk % lot;
    }

    // ---- the opposite side --------------------------------------------------------------------

    /// Levels an order on `side` would take, best first (L2 when present, else the top of book).
    fn opposite(&mut self, slot: u32, side: OrderSide) -> usize {
        let m = &self.markets[slot as usize];
        if let Some(book) = &m.book {
            let out = self.levels.as_mut_slice();
            return match side {
                OrderSide::Buy => book.levels(Side::Ask, out),
                OrderSide::Sell => book.levels(Side::Bid, out),
            };
        }
        let (best, size) = match side {
            OrderSide::Buy => (m.ask, m.ask_size),
            OrderSide::Sell => (m.bid, m.bid_size),
        };
        let (Some(best), Some(def)) = (best, m.instrument) else { return 0 };
        if self.levels.is_empty() {
            return 0;
        }
        self.levels[0] = BookLevel {
            price: best,
            size: Quantity::from_raw(size, def.size_precision).unwrap_or_default(),
        };
        1
    }

    /// Volume available to `side` up to `limit` among the opposite levels.
    fn available(&mut self, slot: u32, side: OrderSide, limit: Option<Price>) -> u64 {
        let n = self.opposite(slot, side);
        self.levels[..n]
            .iter()
            .take_while(|l| within(side, l.price, limit))
            .map(|l| l.size.raw())
            .sum()
    }

    /// Takes up to `want` from the opposite levels.
    fn take(&mut self, index: usize, want: u64) -> Result<()> {
        let Some(o) = self.orders[index] else { return Ok(()) };
        let limit = if o.order_type == OrderType::Market { None } else { Some(o.price) };
        let n = self.opposite(o.slot, o.side);
        let mut taken = 0;
        for i in 0..n {
            if taken >= want {
                break;
            }
            let level = self.levels[i];
            if !within(o.side, level.price, limit) {
                break;
            }
            let q = level.size.raw().min(want - taken);
            if q == 0 {
                continue;
            }
            self.fill(index, q, level.price, LiquiditySide::Taker)?;
            taken += q;
        }
        Ok(())
    }

    /// Resting orders the opposite best has reached fill at their own price.
    fn match_resting(&mut self, slot: u32) -> Result<()> {
        for i in 0..self.orders.len() {
            let Some(o) = self.orders[i] else { continue };
            if o.slot != slot {
                continue;
            }
            let n = self.opposite(slot, o.side);
            if n == 0 || !within(o.side, self.levels[0].price, Some(o.price)) {
                continue;
            }
            let volume: u64 = self.levels[..n]
                .iter()
                .take_while(|l| within(o.side, l.price, Some(o.price)))
                .map(|l| l.size.raw())
                .sum();
            let q = volume.min(o.leaves_raw());
            if q > 0 {
                self.fill(i, q, o.price, LiquiditySide::Maker)?;
            }
        }
        Ok(())
    }

    // ---- commands -----------------------------------------------------------------------------

    fn on_submit(&mut self, c: &SubmitOrder) -> Result<()> {
        let Some(slot) = self.slot_of(&c.instrument_id) else {
            return self.reject_submit(c, reject::UNKNOWN_SYMBOL, false);
        };
        if c.reduce_only && !self.reduces(slot, c.order_side, c.quantity.raw()) {
            return self.reject_submit(c, reject::REDUCE_ONLY, false);
        }
        let limit = if c.order_type == OrderType::Market { None } else { c.price };
        let liquidity = self.available(slot, c.order_side, limit);
        let marketable = liquidity > 0;
        if c.order_type == OrderType::Market && !marketable {
            return self.reject_submit(c, reject::NO_MARKET, false);
        }
        if c.post_only && marketable {
            return self.reject_submit(c, reject::POST_ONLY, true);
        }
        if c.time_in_force == TimeInForce::Fok && liquidity < c.quantity.raw() {
            return self.emit_expired(c.strategy_index, c.instrument_id, c.client_order_id, None);
        }
        let Some(index) = self.add_order(c, slot) else {
            return self.reject_submit(c, reject::CAPACITY, false);
        };
        self.place(index, marketable)
    }

    /// An accepted order: self-trade prevention, the taker part, then rest or expire.
    fn place(&mut self, index: usize, marketable: bool) -> Result<()> {
        let o = self.orders[index].ok_or(Status::InvalidState)?;
        self.emit_accepted(&o)?;
        if marketable && self.config.stp != StpMode::None && self.self_cross(index) {
            let expire_taker =
                matches!(self.config.stp, StpMode::ExpireTaker | StpMode::ExpireBoth);
            if matches!(self.config.stp, StpMode::ExpireMaker | StpMode::ExpireBoth) {
                self.expire_crossed_makers(index)?;
            }
            if expire_taker {
                return self.expire(index);
            }
        }
        if marketable {
            let leaves = o.leaves_raw();
            self.take(index, leaves)?;
        }
        let Some(o) = self.orders[index] else { return Ok(()) }; // filled
        let rests = o.order_type != OrderType::Market
            && matches!(o.time_in_force, TimeInForce::Gtc | TimeInForce::Gtd);
        if !rests {
            return self.expire(index);
        }
        let ahead = self.queue_ahead(&o);
        if let Some(o) = self.orders[index].as_mut() {
            o.ahead_raw = ahead;
        }
        Ok(())
    }

    fn on_modify(&mut self, c: &ModifyOrder) -> Result<()> {
        let Some(index) = self.find(&c.client_order_id) else {
            return self.emit_modify_rejected(c, reject::ORDER_MISSING);
        };
        let o = self.orders[index].ok_or(Status::InvalidState)?;
        if c.quantity.raw() <= o.filled_raw || c.quantity.precision() != o.quantity.precision() {
            return self.emit_modify_rejected(c, reject::QUANTITY_BELOW_FILLED);
        }
        let marketable = self.available(o.slot, o.side, Some(c.price)) > 0;
        if o.post_only && marketable {
            return self.emit_modify_rejected(c, reject::POST_ONLY);
        }
        let keeps_priority = c.price == o.price && c.quantity.raw() <= o.quantity.raw();
        let updated = {
            let o = self.orders[index].as_mut().ok_or(Status::InvalidState)?;
            o.price = c.price;
            o.quantity = c.quantity;
            *o
        };
        self.emit_updated(&updated)?;
        if marketable {
            let leaves = updated.leaves_raw();
            self.take(index, leaves)?;
        }
        if !keeps_priority {
            if let Some(o) = self.orders[index] {
                let ahead = self.queue_ahead(&o);
                if let Some(o) = self.orders[index].as_mut() {
                    o.ahead_raw = ahead;
                }
            }
        }
        Ok(())
    }

    fn on_cancel(&mut self, c: &CancelOrder) -> Result<()> {
        if let Some(index) = self.find(&c.client_order_id) {
            return self.cancel(index);
        }
        let header = self.header(c.strategy_index, c.instrument_id, c.client_order_id);
        let e = OrderCancelRejected {
            header,
            reason: Reason::from_static(reject::UNKNOWN_ORDER),
            venue_order_id: c.venue_order_id,
            account_id: Some(self.config.account_id),
            reconciliation: false,
        };
        self.emit(&OrderEvent::CancelRejected(e))
    }

    fn expire_due(&mut self) -> Result<()> {
        for i in 0..self.orders.len() {
            if self.orders[i].is_some_and(|o| o.expire_time.is_some_and(|t| t <= self.now)) {
                self.expire(i)?;
            }
        }
        Ok(())
    }

    // ---- our orders ---------------------------------------------------------------------------

    fn add_order(&mut self, c: &SubmitOrder, slot: u32) -> Option<usize> {
        let free = self.orders.iter().position(Option::is_none)?;
        self.venue_serial += 1;
        self.orders[free] = Some(SimOrder {
            client_order_id: c.client_order_id,
            venue_order_id: numbered::<VenueOrderId>(b'V', self.venue_serial),
            strategy: c.strategy_index,
            slot,
            instrument_id: c.instrument_id,
            side: c.order_side,
            order_type: c.order_type,
            time_in_force: c.time_in_force,
            post_only: c.post_only,
            reduce_only: c.reduce_only,
            price: c.price.unwrap_or_default(),
            quantity: c.quantity,
            filled_raw: 0,
            expire_time: if c.time_in_force == TimeInForce::Gtd { c.expire_time } else { None },
            ahead_raw: 0,
        });
        Some(free)
    }

    fn find(&self, id: &ClientOrderId) -> Option<usize> {
        self.orders.iter().position(|o| o.is_some_and(|o| o.client_order_id == *id))
    }

    fn slot_of(&self, id: &InstrumentId) -> Option<u32> {
        self.markets
            .iter()
            .position(|m| m.instrument.is_some_and(|i| i.id == *id))
            .map(|i| i as u32)
    }

    /// Volume on the order's own side at its price (the queue it joins).
    fn queue_ahead(&self, o: &SimOrder) -> u64 {
        let m = &self.markets[o.slot as usize];
        if let Some(book) = &m.book {
            return book.size_at(Side::from(o.side), o.price).raw();
        }
        let (best, size) = match o.side {
            OrderSide::Buy => (m.bid, m.bid_size),
            OrderSide::Sell => (m.ask, m.ask_size),
        };
        if best == Some(o.price) {
            size
        } else {
            0
        }
    }

    /// One-way mode: the order must reduce the venue position, and by no more than it.
    fn reduces(&self, slot: u32, side: OrderSide, quantity: u64) -> bool {
        let position = self
            .portfolio
            .venue(InstrumentSlot(slot))
            .map_or(0, portfolio::NettingPosition::signed_raw);
        let opposite = position != 0 && (position > 0) != (side == OrderSide::Buy);
        opposite && quantity <= position.unsigned_abs()
    }

    fn crossed_makers(&self, taker: usize) -> impl Iterator<Item = usize> + '_ {
        let t = self.orders[taker];
        self.orders.iter().enumerate().filter_map(move |(i, o)| {
            let (t, o) = (t?, (*o)?);
            let limit = if t.order_type == OrderType::Market { None } else { Some(t.price) };
            (i != taker && o.slot == t.slot && o.side != t.side && within(t.side, o.price, limit))
                .then_some(i)
        })
    }

    fn self_cross(&self, taker: usize) -> bool {
        self.crossed_makers(taker).next().is_some()
    }

    fn expire_crossed_makers(&mut self, taker: usize) -> Result<()> {
        let mut crossed: FixedVec<usize> = FixedVec::with_capacity(self.orders.len());
        for i in self.crossed_makers(taker) {
            let _ = crossed.push(i);
        }
        for &i in &crossed {
            self.expire(i)?;
        }
        Ok(())
    }

    // ---- fills and events ---------------------------------------------------------------------

    fn fill(
        &mut self,
        index: usize,
        qty_raw: u64,
        price: Price,
        liquidity: LiquiditySide,
    ) -> Result<()> {
        let o = self.orders[index].ok_or(Status::InvalidState)?;
        let def = self.markets[o.slot as usize].instrument.ok_or(Status::InvalidState)?;
        let qty = Quantity::from_raw(qty_raw, o.quantity.precision())?;
        let commission = self.config.fees.commission(&def, price, qty, liquidity)?;
        {
            let order = self.orders[index].as_mut().ok_or(Status::InvalidState)?;
            order.filled_raw += qty_raw;
            if order.leaves_raw() == 0 {
                self.orders[index] = None;
            }
        }
        self.trade_serial += 1;
        let e = OrderFilled {
            header: self.header(o.strategy, o.instrument_id, o.client_order_id),
            venue_order_id: o.venue_order_id,
            account_id: self.config.account_id,
            trade_id: numbered::<TradeId>(b'T', self.trade_serial),
            order_side: o.side,
            order_type: o.order_type,
            last_qty: qty,
            last_px: price,
            currency: def.settlement_currency(),
            liquidity_side: liquidity,
            reconciliation: false,
            position_id: None,
            commission: Some(commission),
            info_flags: model::order_events::FillInfoFlags::default(),
        };
        self.portfolio.on_fill(&def, InstrumentSlot(o.slot), StrategyIndex(0), &e)?;
        self.stats.fills += 1;
        if liquidity == LiquiditySide::Maker {
            self.stats.maker_fills += 1;
        } else {
            self.stats.taker_fills += 1;
        }
        self.emit(&OrderEvent::Filled(e))
    }

    fn cancel(&mut self, index: usize) -> Result<()> {
        let o = self.orders[index].take().ok_or(Status::InvalidState)?;
        let e = OrderCanceled {
            header: self.header(o.strategy, o.instrument_id, o.client_order_id),
            venue_order_id: Some(o.venue_order_id),
            account_id: Some(self.config.account_id),
            reason: None,
            reconciliation: false,
        };
        self.stats.canceled += 1;
        self.emit(&OrderEvent::Canceled(e))
    }

    fn expire(&mut self, index: usize) -> Result<()> {
        let o = self.orders[index].take().ok_or(Status::InvalidState)?;
        self.emit_expired(o.strategy, o.instrument_id, o.client_order_id, Some(o.venue_order_id))
    }

    fn emit_expired(
        &mut self,
        strategy: u16,
        iid: InstrumentId,
        cid: ClientOrderId,
        vid: Option<VenueOrderId>,
    ) -> Result<()> {
        let e = OrderExpired {
            header: self.header(strategy, iid, cid),
            venue_order_id: vid,
            account_id: Some(self.config.account_id),
            reconciliation: false,
        };
        self.stats.expired += 1;
        self.emit(&OrderEvent::Expired(e))
    }

    fn emit_accepted(&mut self, o: &SimOrder) -> Result<()> {
        let e = OrderAccepted {
            header: self.header(o.strategy, o.instrument_id, o.client_order_id),
            venue_order_id: o.venue_order_id,
            account_id: self.config.account_id,
            reconciliation: false,
        };
        self.emit(&OrderEvent::Accepted(e))
    }

    fn emit_updated(&mut self, o: &SimOrder) -> Result<()> {
        let e = OrderUpdated {
            header: self.header(o.strategy, o.instrument_id, o.client_order_id),
            venue_order_id: Some(o.venue_order_id),
            account_id: Some(self.config.account_id),
            quantity: o.quantity,
            price: Some(o.price),
            trigger_price: None,
            protection_price: None,
            is_quote_quantity: false,
            reconciliation: false,
        };
        self.emit(&OrderEvent::Updated(e))
    }

    fn emit_modify_rejected(&mut self, c: &ModifyOrder, reason: &'static str) -> Result<()> {
        let e = OrderModifyRejected {
            header: self.header(c.strategy_index, c.instrument_id, c.client_order_id),
            reason: Reason::from_static(reason),
            venue_order_id: c.venue_order_id,
            account_id: Some(self.config.account_id),
            reconciliation: false,
        };
        self.emit(&OrderEvent::ModifyRejected(e))
    }

    fn reject_submit(
        &mut self,
        c: &SubmitOrder,
        reason: &'static str,
        due_post_only: bool,
    ) -> Result<()> {
        let e = OrderRejected {
            header: self.header(c.strategy_index, c.instrument_id, c.client_order_id),
            account_id: self.config.account_id,
            reason: Reason::from_static(reason),
            reconciliation: false,
            due_post_only,
        };
        self.stats.rejected += 1;
        self.emit(&OrderEvent::Rejected(e))
    }

    fn header(&mut self, strategy: u16, iid: InstrumentId, cid: ClientOrderId) -> OrderEventHeader {
        self.event_serial += 1;
        OrderEventHeader {
            trader_id: self.config.trader_id,
            strategy_id: self.strategy_ids.get(strategy as usize).copied().unwrap_or_default(),
            instrument_id: iid,
            client_order_id: cid,
            event_id: Uuid4::from_u64s(
                self.rng.draw(self.event_serial, 1, 0),
                self.rng.draw(self.event_serial, 2, 0),
            ),
            ts_event: self.now,
            ts_init: self.now,
            causation_id: None,
        }
    }

    fn emit(&mut self, e: &OrderEvent) -> Result<()> {
        self.out.push(*e)
    }
}

/// `V123`, `T45`: numbered venue ids.
fn numbered<T: core::str::FromStr + Default>(prefix: u8, n: u64) -> T {
    let mut buf = [0u8; 21];
    buf[0] = prefix;
    let mut digits = [0u8; 20];
    let mut d = 0;
    let mut n = n;
    loop {
        digits[d] = b'0' + (n % 10) as u8;
        d += 1;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    for i in 0..d {
        buf[1 + i] = digits[d - 1 - i];
    }
    core::str::from_utf8(&buf[..=d]).ok().and_then(|s| s.parse().ok()).unwrap_or_default()
}
