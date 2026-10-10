//! The kernel's trading state and command path (docs/architecture.md sections 8 and 9): the OMS,
//! the identities orders are issued under, the portfolio, the risk engine, and the order events
//! the kernel itself produced in this step.
//!
//! - `submit` assigns the `ClientOrderId`, checks the intent, then either denies it (`OrderDenied`
//!   output and event) or marks it `Submitted` (`SubmitOrder` output, `OrderSubmitted` event);
//! - `modify`: `PendingUpdate`, `ModifyOrder` output, `OrderPendingUpdate` event; a modify the
//!   gate refuses comes back as `OrderModifyRejected` and the order stays as it was;
//! - `cancel`: `PendingCancel`, `CancelOrder` output, `OrderPendingCancel` event.
//!
//! A command either happens completely or not at all: the capacity it needs (one output, one
//! event) is checked before anything changes. The events wait in `events` until the callback that
//! caused them returns; the engine then delivers them. Venue events (accepted, filled, canceled,
//! ...) are applied by `on_venue_event` and delivered by the engine directly. Denials are events,
//! not errors: `submit` returns `Ok` for a denied order.

use data::intern::{InstrumentSlot, InstrumentTable};
use data::subscription::StrategyIndex;
use execution::{
    apply_order_event, next_status, EventOutcome, Oms, OrderEventKind, OrderIndex, OrderIntent,
    OrderRecord,
};
use kernel_core::rng::CounterRng;
use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{EventKey, FixedString, FixedVec, Result, Status, UnixNanos};
use model::account::AccountBalance;
use model::client_order_id::Generator;
use model::data::FundingRateUpdate;
use model::enums::{NodeState, OrderSide, OrderStatus, OrderType, PositionAdjustmentType};
use model::instruments::Instrument;
use model::order_events::{
    OrderDenied, OrderEvent, OrderEventHeader, OrderFillVoided, OrderFilled, OrderModifyRejected,
    OrderPendingCancel, OrderPendingUpdate, OrderSubmitted, Reason,
};
use model::outputs::{CancelOrder, ModifyOrder, Output, SubmitOrder};
use model::position_events::{
    PositionAdjusted, PositionChanged, PositionClosed, PositionEvent, PositionEventHeader,
    PositionOpened, PositionState,
};
use model::{
    AccountId, ClientOrderId, Currency, InstrumentId, Money, PositionId, Price, Quantity,
    StrategyId, TraderId, Uuid4, FIXED_PRECISION,
};
use portfolio::{MarginModel, NettingPosition, Portfolio, PortfolioConfig, PositionStep};
use risk::gates::{classify, MarginCheck};
use risk::{check_intent, checks, CommandKind, OrderCheck, RiskConfig, RiskEngine, TradingTrigger};

use crate::views::{ExposureView, OrderView, PositionView};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TradingConfig {
    /// Live and recently closed orders.
    pub orders: u32,
    /// Fill records for duplicate detection.
    pub trades: u32,
    /// Kernel-produced order and position events per step.
    pub order_events: u32,
    pub trader_id: TraderId,
    pub account_id: AccountId,
    /// `ClientOrderId` prefix.
    pub node_tag: FixedString<8>,
    /// `ClientOrderId` epoch (always 1 in backtest).
    pub epoch: u64,
    /// Balances the portfolio tracks.
    pub currencies: u32,
    pub margin: MarginModel,
    pub risk: RiskConfig,
}

impl Default for TradingConfig {
    fn default() -> Self {
        Self {
            orders: 4096,
            trades: 65536,
            order_events: 1024,
            trader_id: TraderId::new("JARVIS-001").unwrap_or_default(),
            account_id: AccountId::new("SIM-001").unwrap_or_default(),
            node_tag: FixedString::from_static("jarvis"),
            epoch: 1,
            currencies: 16,
            margin: MarginModel::Standard,
            risk: RiskConfig::default(),
        }
    }
}

/// An event the kernel produced for a strategy in this step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KernelEvent {
    Order(OrderEvent),
    Position(PositionEvent),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PendingEvent {
    pub strategy: StrategyIndex,
    pub event: KernelEvent,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TradingStats {
    pub submitted: u64,
    pub denied: u64,
    pub modifies: u64,
    pub modify_rejected: u64,
    pub cancels: u64,
    /// Applied venue order events.
    pub venue_events: u64,
    pub unknown_order_events: u64,
    pub refused_order_events: u64,
    pub stale_order_events: u64,
    pub duplicate_fills: u64,
    /// Kernel events lost to a full queue (position events).
    pub dropped_events: u64,
    pub kill_switches: u64,
}
kernel_core::state_fields!(TradingStats {
    submitted,
    denied,
    modifies,
    modify_rejected,
    cancels,
    venue_events,
    unknown_order_events,
    refused_order_events,
    stale_order_events,
    duplicate_fills,
    dropped_events,
    kill_switches
});

pub type Outputs = FixedVec<Output>;

/// Event ids draw from their own Philox key, apart from the strategies' `ctx.rng` streams.
const EVENT_ID_SALT: u64 = 0x6A09_E667_F3BC_C909;

#[derive(Clone, Debug)]
pub struct Trading {
    pub oms: Oms,
    pub ids: Generator,
    pub trader_id: TraderId,
    pub account_id: AccountId,
    pub strategy_ids: FixedVec<StrategyId>,
    /// Kernel-produced events, delivered after the callback that caused them.
    pub events: FixedVec<PendingEvent>,
    pub portfolio: Portfolio,
    pub risk: RiskEngine,
    pub stats: TradingStats,
    event_rng: CounterRng,
    event_serial: u32,
}

/// `strategy-001`, `strategy-002`, ...: the id of strategy `index` unless the node names it.
#[must_use]
pub fn default_strategy_id(index: u16) -> StrategyId {
    let n = u32::from(index) + 1;
    let mut text = *b"strategy-000";
    text[9] = b'0' + ((n / 100) % 10) as u8;
    text[10] = b'0' + ((n / 10) % 10) as u8;
    text[11] = b'0' + (n % 10) as u8;
    StrategyId::new(core::str::from_utf8(&text).unwrap_or("strategy")).unwrap_or_default()
}

impl Trading {
    #[must_use]
    pub fn new(c: &TradingConfig, instruments: u32, strategies: u16, seed: u64) -> Self {
        let tag = if c.node_tag.is_empty() { "jarvis" } else { c.node_tag.as_str() };
        let ids = Generator::new(tag, c.epoch)
            .or_else(|_| Generator::new("jarvis", 1))
            .unwrap_or_else(|_| Generator::new("jarvis", 1).expect("a valid default tag"));
        Self {
            oms: Oms::new(c.orders, c.trades, instruments),
            ids,
            trader_id: c.trader_id,
            account_id: c.account_id,
            strategy_ids: FixedVec::from_iter_exact((0..strategies).map(default_strategy_id)),
            events: FixedVec::with_capacity(c.order_events as usize),
            portfolio: Portfolio::new(PortfolioConfig {
                instruments,
                strategies,
                margin: c.margin,
                currencies: c.currencies,
            }),
            risk: RiskEngine::new(c.risk, instruments, strategies),
            stats: TradingStats::default(),
            event_rng: CounterRng::new(seed ^ EVENT_ID_SALT),
            event_serial: 0,
        }
    }

    /// Called by the engine before each input.
    pub fn begin_step(&mut self) {
        self.event_serial = 0;
    }

    pub fn set_strategy_id(&mut self, s: StrategyIndex, id: StrategyId) {
        if let Some(slot) = self.strategy_ids.get_mut(s.0 as usize) {
            *slot = id;
        }
    }
    #[must_use]
    pub fn strategy_id(&self, s: StrategyIndex) -> StrategyId {
        self.strategy_ids.get(s.0 as usize).copied().unwrap_or_default()
    }

    // ---- commands -----------------------------------------------------------------------------

    /// Submits an order of strategy `s`; `slot` is the instrument's slot when the kernel has seen
    /// the instrument. `Ok` with the id when the order was submitted or denied (an `OrderDenied`
    /// names it); `CapacityExceeded` when the step has no room for the output or the event.
    pub fn submit(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        intent: &OrderIntent,
        slot: Option<InstrumentSlot>,
        instruments: &InstrumentTable,
        outputs: &mut Outputs,
    ) -> Result<ClientOrderId> {
        if !self.room(outputs) {
            return Err(Status::CapacityExceeded);
        }
        let cid = self.ids.next_id()?;
        let mut record = OrderRecord::new(
            cid,
            s.0,
            slot.map_or(u32::MAX, |s| s.0),
            intent.instrument_id,
            intent.side,
            intent.order_type,
            intent.quantity,
            intent.price,
            intent.time_in_force,
            now.ts,
        );
        record.post_only = intent.post_only;
        record.reduce_only = intent.reduce_only;
        record.expire_time = intent.expire_time;
        let definition = slot.and_then(|slot| instruments.instrument(slot));
        let mut reason = match definition {
            None => Some(checks::INSTRUMENT_UNKNOWN),
            Some(def) => check_intent(def, intent, now.ts),
        };
        let index = self.oms.create(record).ok();
        if index.is_none() {
            reason = reason.or(Some(checks::OMS_CAPACITY_EXCEEDED));
        }
        if reason.is_none() {
            if let (Some(def), Some(slot)) = (definition, slot) {
                let check = self.order_check(
                    now,
                    s,
                    slot,
                    def,
                    intent.side,
                    intent.order_type,
                    intent.quantity,
                    intent.price,
                    intent.reduce_only,
                    None,
                    instruments,
                );
                reason = self.risk.check_order(&check);
            }
        }
        if let Some(reason) = reason {
            self.deny(now, s, &record, index, reason, outputs);
            return Ok(cid);
        }
        let index = index.ok_or(Status::InvalidState)?;
        self.oms.apply(index, OrderEventKind::Submitted)?;
        let _ = outputs.push(Output::SubmitOrder(SubmitOrder {
            strategy_index: s.0,
            client_order_id: cid,
            instrument_id: intent.instrument_id,
            order_side: intent.side,
            order_type: intent.order_type,
            quantity: intent.quantity,
            price: intent.price,
            time_in_force: intent.time_in_force,
            post_only: intent.post_only,
            reduce_only: intent.reduce_only,
            expire_time: intent.expire_time,
            ts_init: now.ts,
        }));
        let header = self.header(now, s, &record);
        let _ = self.events.push(PendingEvent {
            strategy: s,
            event: KernelEvent::Order(OrderEvent::Submitted(OrderSubmitted {
                header,
                account_id: self.account_id,
            })),
        });
        self.stats.submitted += 1;
        Ok(cid)
    }

    /// A `LIMIT` order's new quantity and/or price. `NotFound` for an order this strategy does not
    /// own; `InvalidState` when the order cannot be modified now (closed, or a cancel is pending);
    /// `InvalidArgument` for a market order, a price off the instrument's grid or a quantity not
    /// above what is filled. A modify the gate refuses is not an error: `OrderModifyRejected`
    /// arrives as an event.
    #[allow(clippy::too_many_arguments)]
    pub fn modify(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        cid: &ClientOrderId,
        quantity: Option<Quantity>,
        price: Option<Price>,
        instruments: &InstrumentTable,
        outputs: &mut Outputs,
    ) -> Result<()> {
        let index = self.owned(s, cid).ok_or(Status::NotFound)?;
        let r = *self.oms.get(index).ok_or(Status::NotFound)?;
        let def = instruments.instrument(InstrumentSlot(r.slot)).ok_or(Status::InvalidArgument)?;
        let (Some(current_price), OrderType::Limit) = (r.price, r.order_type) else {
            return Err(Status::InvalidArgument);
        };
        if quantity.is_none() && price.is_none() {
            return Err(Status::InvalidArgument);
        }
        let q = quantity.unwrap_or(r.state.quantity());
        let p = price.unwrap_or(current_price);
        if checks::check_price(def, p).is_some()
            || q.precision() != r.state.quantity().precision()
            || q.raw() <= r.state.filled().raw()
        {
            return Err(Status::InvalidArgument);
        }
        if !Self::can(&r, OrderEventKind::PendingUpdate) {
            return Err(Status::InvalidState);
        }
        if !self.room(outputs) {
            return Err(Status::CapacityExceeded);
        }
        let check = self.order_check(
            now,
            s,
            InstrumentSlot(r.slot),
            def,
            r.side,
            r.order_type,
            q,
            Some(p),
            r.reduce_only,
            Some(r.state.quantity()),
            instruments,
        );
        if let Some(denied) = self.risk.check_modify(&check) {
            let header = self.header(now, s, &r);
            let _ = self.events.push(PendingEvent {
                strategy: s,
                event: KernelEvent::Order(OrderEvent::ModifyRejected(OrderModifyRejected {
                    header,
                    reason: Reason::from_text(denied).unwrap_or_default(),
                    venue_order_id: r.venue_order_id,
                    account_id: Some(self.account_id),
                    reconciliation: false,
                })),
            });
            self.stats.modify_rejected += 1;
            return Ok(());
        }
        self.oms.apply(index, OrderEventKind::PendingUpdate)?;
        let _ = outputs.push(Output::ModifyOrder(ModifyOrder {
            strategy_index: s.0,
            client_order_id: r.client_order_id,
            instrument_id: r.instrument_id,
            venue_order_id: r.venue_order_id,
            quantity: q,
            price: p,
            ts_init: now.ts,
        }));
        let header = self.header(now, s, &r);
        let _ = self.events.push(PendingEvent {
            strategy: s,
            event: KernelEvent::Order(OrderEvent::PendingUpdate(OrderPendingUpdate {
                header,
                account_id: self.account_id,
                venue_order_id: r.venue_order_id,
                reconciliation: false,
            })),
        });
        self.stats.modifies += 1;
        Ok(())
    }

    /// `NotFound` for an order this strategy does not own; `InvalidState` when it is closed or a
    /// cancel is already pending.
    pub fn cancel(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        cid: &ClientOrderId,
        outputs: &mut Outputs,
    ) -> Result<()> {
        let index = self.owned(s, cid).ok_or(Status::NotFound)?;
        self.cancel_index(now, s, index, outputs)
    }

    /// Cancels every cancelable order of the strategy (of one instrument when `id` is given);
    /// returns how many cancels were sent.
    pub fn cancel_all(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        id: Option<&InstrumentId>,
        outputs: &mut Outputs,
    ) -> Result<u32> {
        let mut canceled = 0;
        for i in 0..self.oms.capacity() as u32 {
            let index = OrderIndex(i);
            let Some(r) = self.oms.get(index) else { continue };
            if r.strategy != s.0
                || id.is_some_and(|id| *id != r.instrument_id)
                || !Self::can(r, OrderEventKind::PendingCancel)
            {
                continue;
            }
            self.cancel_index(now, s, index, outputs)?;
            canceled += 1;
        }
        Ok(canceled)
    }

    /// Cancels every cancelable order of every strategy (the kill switch, section 10.3).
    pub fn kill_switch(&mut self, now: EventKey, outputs: &mut Outputs) -> Result<u32> {
        self.risk.note_kill_switch();
        self.stats.kill_switches += 1;
        let mut canceled = 0;
        for s in 0..self.strategy_ids.len() as u16 {
            canceled += self.cancel_all(now, StrategyIndex(s), None, outputs)?;
        }
        Ok(canceled)
    }

    /// Node lifecycle -> `TradingState` holds (section 10.2).
    pub fn on_lifecycle(&mut self, from: NodeState, to: NodeState) {
        match to {
            NodeState::Syncing => {
                if from == NodeState::Degraded {
                    self.risk.apply(TradingTrigger::Recovered);
                }
                self.risk.apply(TradingTrigger::SyncStarted);
            }
            NodeState::Running => {
                self.risk.apply(TradingTrigger::Synced);
            }
            NodeState::Degraded => {
                self.risk.apply(TradingTrigger::Degraded);
            }
            _ => {}
        }
    }

    // ---- venue events -------------------------------------------------------------------------

    /// Applies an order event from the venue; the owning strategy when it was applied and the
    /// strategy should receive it. An applied fill (or fill void) is booked in the portfolio first,
    /// so the strategy sees its new position during `on_order_event`; the position events it
    /// causes are queued behind it.
    pub fn on_venue_event(
        &mut self,
        now: EventKey,
        event: &OrderEvent,
        instruments: &InstrumentTable,
    ) -> Result<Option<StrategyIndex>> {
        let (outcome, index) = apply_order_event(&mut self.oms, event);
        match outcome {
            EventOutcome::Applied => {
                self.stats.venue_events += 1;
                let index = index.ok_or(Status::InvalidState)?;
                let record = *self.oms.get(index).ok_or(Status::InvalidState)?;
                self.book(now, &record, event, instruments)?;
                Ok(Some(StrategyIndex(record.strategy)))
            }
            EventOutcome::UnknownOrder => {
                self.stats.unknown_order_events += 1;
                Ok(None)
            }
            EventOutcome::DuplicateFill => {
                self.stats.duplicate_fills += 1;
                if let (Some(index), OrderEvent::Filled(f)) = (index, event) {
                    self.book_late_commission(index, f)?;
                }
                Ok(None)
            }
            EventOutcome::Refused => {
                self.stats.refused_order_events += 1;
                Ok(None)
            }
            EventOutcome::Stale => {
                self.stats.stale_order_events += 1;
                Ok(None)
            }
        }
    }

    fn book(
        &mut self,
        now: EventKey,
        r: &OrderRecord,
        event: &OrderEvent,
        instruments: &InstrumentTable,
    ) -> Result<()> {
        let slot = InstrumentSlot(r.slot);
        let Some(def) = instruments.instrument(slot) else { return Ok(()) };
        let s = StrategyIndex(r.strategy);
        match event {
            OrderEvent::Filled(f) => {
                let Some(outcome) = self.portfolio.on_fill(def, slot, s, f)? else { return Ok(()) };
                for part in outcome.iter() {
                    let position = *self.portfolio.position(s, slot).ok_or(Status::OutOfRange)?;
                    let state = Self::position_state(
                        def,
                        &position,
                        f.order_side,
                        f.last_qty,
                        f.last_px,
                        part.realized,
                        &f.header.client_order_id,
                    )?;
                    let header = self.position_header(now, s, def, f.header.ts_event)?;
                    let event = match part.step {
                        PositionStep::Opened => {
                            PositionEvent::Opened(PositionOpened { header, state })
                        }
                        PositionStep::Changed => PositionEvent::Changed(PositionChanged {
                            header,
                            state,
                            ts_opened: position.ts_opened(),
                        }),
                        PositionStep::Closed => PositionEvent::Closed(PositionClosed {
                            header,
                            state,
                            closing_order_id: f.header.client_order_id,
                            ts_opened: position.ts_opened(),
                            ts_closed: f.header.ts_event,
                            duration: f
                                .header
                                .ts_event
                                .since(position.ts_opened())
                                .unwrap_or_default(),
                        }),
                    };
                    self.queue(s, &KernelEvent::Position(event));
                }
                Ok(())
            }
            OrderEvent::FillVoided(v) => self.book_void(now, def, slot, s, v),
            _ => Ok(()),
        }
    }

    /// A voided fill is booked as the opposite fill at the same price.
    fn book_void(
        &mut self,
        now: EventKey,
        def: &Instrument,
        slot: InstrumentSlot,
        s: StrategyIndex,
        v: &OrderFillVoided,
    ) -> Result<()> {
        let reverse = OrderFilled {
            header: v.header,
            venue_order_id: v.venue_order_id,
            account_id: v.account_id,
            trade_id: v.correction_id,
            order_side: v.order_side.opposite(),
            order_type: v.order_type,
            last_qty: v.voided_qty,
            last_px: v.last_px,
            currency: v.currency,
            liquidity_side: v.liquidity_side,
            reconciliation: v.reconciliation,
            position_id: v.position_id,
            commission: v.commission_voided.map(Money::negated),
            info_flags: v.info_flags,
        };
        let Some(outcome) = self.portfolio.on_fill(def, slot, s, &reverse)? else { return Ok(()) };
        for part in outcome.iter() {
            let position = *self.portfolio.position(s, slot).ok_or(Status::OutOfRange)?;
            let state = Self::position_state(
                def,
                &position,
                reverse.order_side,
                reverse.last_qty,
                reverse.last_px,
                part.realized,
                &v.header.client_order_id,
            )?;
            let header = self.position_header(now, s, def, v.header.ts_event)?;
            self.queue(
                s,
                &KernelEvent::Position(PositionEvent::Changed(PositionChanged {
                    header,
                    state,
                    ts_opened: position.ts_opened(),
                })),
            );
        }
        Ok(())
    }

    /// The commission of a Lite fill arriving with its full report (section 8.3).
    fn book_late_commission(&mut self, index: OrderIndex, f: &OrderFilled) -> Result<()> {
        let Some(commission) = f.commission else { return Ok(()) };
        let r = *self.oms.get(index).ok_or(Status::NotFound)?;
        if self.oms.take_pending_commission(index, &f.trade_id) {
            self.portfolio.on_commission(
                InstrumentSlot(r.slot),
                StrategyIndex(r.strategy),
                commission,
            )?;
        }
        Ok(())
    }

    /// A funding rate update; a settlement queues a `PositionAdjusted` for every strategy holding
    /// the instrument.
    pub fn on_funding(
        &mut self,
        now: EventKey,
        slot: InstrumentSlot,
        update: &FundingRateUpdate,
        instruments: &InstrumentTable,
    ) -> Result<()> {
        let Some(def) = instruments.instrument(slot) else { return Ok(()) };
        let Some(settlement) = self.portfolio.on_funding(def, slot, update)? else { return Ok(()) };
        for &(s, share) in &settlement.shares {
            let header = self.position_header(now, s, def, update.ts_event)?;
            self.queue(
                s,
                &KernelEvent::Position(PositionEvent::Adjusted(PositionAdjusted {
                    header,
                    adjustment_type: PositionAdjustmentType::Funding,
                    quantity_change: 0,
                    pnl_change: share,
                    reason: None,
                })),
            );
        }
        Ok(())
    }

    // ---- queries ------------------------------------------------------------------------------

    #[must_use]
    pub fn order(
        &self,
        s: StrategyIndex,
        cid: &ClientOrderId,
        instruments: &InstrumentTable,
    ) -> Option<OrderView> {
        let index = self.owned(s, cid)?;
        let r = self.oms.get(index)?;
        Some(Self::view(r, instruments))
    }

    /// The strategy's open orders (of one instrument when `id` is given), oldest slot first:
    /// writes up to `out.len()` of them and returns how many there are.
    pub fn open_orders(
        &self,
        s: StrategyIndex,
        id: Option<&InstrumentId>,
        instruments: &InstrumentTable,
        out: &mut [OrderView],
    ) -> usize {
        let mut n = 0;
        for (_, r) in self.oms.iter() {
            if r.strategy != s.0 || !r.is_open() || id.is_some_and(|id| *id != r.instrument_id) {
                continue;
            }
            if let Some(slot) = out.get_mut(n) {
                *slot = Self::view(r, instruments);
            }
            n += 1;
        }
        n
    }

    fn view(r: &OrderRecord, instruments: &InstrumentTable) -> OrderView {
        let precision = instruments
            .instrument(InstrumentSlot(r.slot))
            .map_or(FIXED_PRECISION, |d| d.price_precision);
        OrderView {
            client_order_id: r.client_order_id,
            venue_order_id: r.venue_order_id,
            instrument_id: r.instrument_id,
            side: r.side,
            order_type: r.order_type,
            time_in_force: r.time_in_force,
            post_only: r.post_only,
            reduce_only: r.reduce_only,
            price: r.price,
            status: r.state.status(),
            quantity: r.state.quantity(),
            filled: r.state.filled(),
            leaves: r.state.leaves(),
            avg_px: r.average_price(precision),
            ts_init: r.ts_init,
        }
    }

    /// The strategy's position in instrument `slot`; `None` without an instrument definition.
    #[must_use]
    pub fn position(
        &self,
        s: StrategyIndex,
        slot: InstrumentSlot,
        instruments: &InstrumentTable,
    ) -> Option<PositionView> {
        let def = instruments.instrument(slot)?;
        let p = self.portfolio.position(s, slot)?;
        let ccy = def.settlement_currency();
        let money = |raw: i64| Money::from_raw(raw, ccy).unwrap_or(Money::zero(ccy));
        Some(PositionView {
            instrument_id: def.id,
            position_id: PositionId::netting(&def.id, &self.strategy_id(s)).unwrap_or_default(),
            side: p.side(),
            signed_raw: p.signed_raw(),
            quantity: Quantity::from_raw(p.quantity_raw(), FIXED_PRECISION).unwrap_or_default(),
            avg_px_open: p.avg_px_open(),
            realized_pnl: money(p.realized_raw() - p.commission_raw() + p.funding_raw()),
            unrealized_pnl: self.portfolio.unrealized(def, slot, p).unwrap_or(Money::zero(ccy)),
            commission: money(p.commission_raw()),
            funding: money(p.funding_raw()),
            total_pnl: money(
                p.total_realized_raw() - p.total_commission_raw() + p.total_funding_raw(),
            ),
            ts_opened: p.ts_opened(),
        })
    }

    /// `open_exposure()` of instrument `slot`; `None` without an instrument definition.
    #[must_use]
    pub fn exposure(
        &self,
        slot: InstrumentSlot,
        instruments: &InstrumentTable,
    ) -> Option<ExposureView> {
        let def = instruments.instrument(slot)?;
        let open = self.oms.open_quantity(slot.0, None);
        let position = self.portfolio.venue(slot)?.signed_raw();
        let clamp = |v: i128| i64::try_from(v).unwrap_or(if v < 0 { i64::MIN } else { i64::MAX });
        let max_long = clamp(i128::from(position) + i128::from(open.buy_raw));
        let max_short = clamp(i128::from(position) - i128::from(open.sell_raw));
        let worst = max_long.unsigned_abs().max(max_short.unsigned_abs());
        let notional = self.portfolio.valuation(slot).and_then(|price| {
            let q = Quantity::from_raw(worst, FIXED_PRECISION).ok()?;
            let raw = model::fixed_point::notional_raw(price, q, def.multiplier).ok()?;
            Money::from_raw(raw, def.quote_currency).ok()
        });
        Some(ExposureView {
            instrument_id: def.id,
            position_raw: position,
            open_buy: Quantity::from_raw(open.buy_raw, FIXED_PRECISION).unwrap_or_default(),
            open_sell: Quantity::from_raw(open.sell_raw, FIXED_PRECISION).unwrap_or_default(),
            max_long_raw: max_long,
            max_short_raw: max_short,
            notional,
        })
    }

    /// Balance of `currency`: total is the wallet balance, locked the initial margin of the venue
    /// positions settled in it, free the rest. `None` when the account never held it.
    #[must_use]
    pub fn balance(
        &self,
        currency: Currency,
        instruments: &InstrumentTable,
    ) -> Option<AccountBalance> {
        let wallet = self.portfolio.wallet(currency)?;
        let (_, initial, _) = self.exposure_totals(currency, instruments);
        let locked = Money::from_raw(initial, currency).ok()?;
        let free = Money::from_raw(wallet.raw() - locked.raw(), currency).ok()?;
        AccountBalance::new(wallet, locked, free).ok()
    }

    /// Available margin in `currency` (10^9 raw): wallet + unrealized PnL - initial margin of the
    /// venue positions and of the open orders settled in it. `None` when the account never held
    /// it.
    #[must_use]
    pub fn available(&self, currency: Currency, instruments: &InstrumentTable) -> Option<i64> {
        let wallet = self.portfolio.wallet(currency)?;
        let (unrealized, initial, _) = self.exposure_totals(currency, instruments);
        Some(wallet.raw() + unrealized - initial)
    }

    /// Unrealized PnL and initial and maintenance margin of every venue position and open order
    /// settled in `currency` (10^9 raw).
    fn exposure_totals(
        &self,
        currency: Currency,
        instruments: &InstrumentTable,
    ) -> (i64, i64, i64) {
        let (mut unrealized, mut initial, mut maintenance) = (0i64, 0i64, 0i64);
        for slot in 0..instruments.capacity() as u32 {
            let slot = InstrumentSlot(slot);
            let Some(def) = instruments.instrument(slot) else { continue };
            if def.settlement_currency() != currency {
                continue;
            }
            if let Some(p) = self.portfolio.venue(slot).filter(|p| p.is_open()) {
                if let Ok(u) = self.portfolio.unrealized(def, slot, p) {
                    unrealized += u.raw();
                }
                if let Ok((im, mm)) = self.portfolio.margins(def, slot) {
                    initial += im.raw();
                    maintenance += mm.raw();
                }
            }
            let open = self.oms.open_quantity(slot.0, None);
            let notional_18 = open.buy_notional + open.sell_notional;
            if notional_18 != 0 {
                // notional x multiplier / 10^18 is the 10^9 raw notional of the open orders.
                let scaled =
                    notional_18 / 1_000_000_000 * u128::from(def.multiplier.raw()) / 1_000_000_000;
                if let Ok(raw) = u64::try_from(scaled) {
                    if let Ok(m) = self.portfolio.config().margin.initial(def, raw) {
                        initial += m.raw();
                    }
                }
            }
        }
        (unrealized, initial, maintenance)
    }

    // ---- internals ----------------------------------------------------------------------------

    fn room(&self, outputs: &Outputs) -> bool {
        !outputs.is_full() && !self.events.is_full()
    }

    fn can(r: &OrderRecord, kind: OrderEventKind) -> bool {
        let from = r.state.status();
        if kind == OrderEventKind::PendingCancel && from == OrderStatus::PendingCancel {
            return false; // one cancel in flight is enough
        }
        next_status(from, kind).is_some()
    }

    fn owned(&self, s: StrategyIndex, cid: &ClientOrderId) -> Option<OrderIndex> {
        let index = self.oms.find(cid)?;
        (self.oms.get(index)?.strategy == s.0).then_some(index)
    }

    fn cancel_index(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        index: OrderIndex,
        outputs: &mut Outputs,
    ) -> Result<()> {
        let r = *self.oms.get(index).ok_or(Status::NotFound)?;
        if !Self::can(&r, OrderEventKind::PendingCancel) {
            return Err(Status::InvalidState);
        }
        if !self.room(outputs) {
            return Err(Status::CapacityExceeded);
        }
        self.oms.apply(index, OrderEventKind::PendingCancel)?;
        let _ = outputs.push(Output::CancelOrder(CancelOrder {
            strategy_index: s.0,
            client_order_id: r.client_order_id,
            instrument_id: r.instrument_id,
            venue_order_id: r.venue_order_id,
            ts_init: now.ts,
        }));
        let header = self.header(now, s, &r);
        let _ = self.events.push(PendingEvent {
            strategy: s,
            event: KernelEvent::Order(OrderEvent::PendingCancel(OrderPendingCancel {
                header,
                account_id: self.account_id,
                venue_order_id: r.venue_order_id,
                reconciliation: false,
            })),
        });
        self.stats.cancels += 1;
        Ok(())
    }

    /// `index` is `None` when the OMS had no room for the order.
    fn deny(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        record: &OrderRecord,
        index: Option<OrderIndex>,
        reason: &str,
        outputs: &mut Outputs,
    ) {
        if let Some(index) = index {
            let _ = self.oms.apply(index, OrderEventKind::Denied);
        }
        let denied = OrderDenied {
            header: self.header(now, s, record),
            reason: Reason::from_text(reason).unwrap_or_default(),
        };
        let _ = outputs.push(Output::OrderDenied(denied));
        let _ = self.events.push(PendingEvent {
            strategy: s,
            event: KernelEvent::Order(OrderEvent::Denied(denied)),
        });
        self.stats.denied += 1;
    }

    fn queue(&mut self, s: StrategyIndex, event: &KernelEvent) {
        if self.events.push(PendingEvent { strategy: s, event: *event }).is_err() {
            self.stats.dropped_events += 1;
        }
    }

    fn next_event_id(&mut self, now: EventKey) -> Uuid4 {
        let serial = self.event_serial;
        self.event_serial += 1;
        Uuid4::from_u64s(
            self.event_rng.draw(now.seq, 1, serial),
            self.event_rng.draw(now.seq, 2, serial),
        )
    }

    fn header(&mut self, now: EventKey, s: StrategyIndex, r: &OrderRecord) -> OrderEventHeader {
        OrderEventHeader {
            trader_id: self.trader_id,
            strategy_id: self.strategy_id(s),
            instrument_id: r.instrument_id,
            client_order_id: r.client_order_id,
            event_id: self.next_event_id(now),
            ts_event: now.ts,
            ts_init: now.ts,
            causation_id: None,
        }
    }

    fn position_header(
        &mut self,
        now: EventKey,
        s: StrategyIndex,
        def: &Instrument,
        ts_event: UnixNanos,
    ) -> Result<PositionEventHeader> {
        Ok(PositionEventHeader {
            trader_id: self.trader_id,
            strategy_id: self.strategy_id(s),
            instrument_id: def.id,
            position_id: PositionId::netting(&def.id, &self.strategy_id(s))?,
            account_id: self.account_id,
            event_id: self.next_event_id(now),
            ts_event,
            ts_init: now.ts,
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn position_state(
        def: &Instrument,
        p: &NettingPosition,
        side: OrderSide,
        last_qty: Quantity,
        last_px: Price,
        realized: Money,
        order_id: &ClientOrderId,
    ) -> Result<PositionState> {
        let ccy = def.settlement_currency();
        Ok(PositionState {
            opening_order_id: p.opening_order_id().copied().unwrap_or(*order_id),
            entry: p.entry().unwrap_or(side),
            side: p.side(),
            signed_qty: p.signed_raw(),
            quantity: Quantity::from_raw(p.quantity_raw(), FIXED_PRECISION)?,
            peak_quantity: Quantity::from_raw(p.peak_raw(), FIXED_PRECISION)?,
            last_qty,
            last_px,
            currency: ccy,
            avg_px_open: p.avg_px_open().unwrap_or(last_px),
            avg_px_close: p.avg_px_close(),
            realized_return: 0,
            realized_pnl: realized,
            unrealized_pnl: None,
        })
    }

    /// What the gates see of an order. `replaced` is the quantity a modify replaces.
    #[allow(clippy::too_many_arguments)]
    fn order_check<'a>(
        &self,
        now: EventKey,
        s: StrategyIndex,
        slot: InstrumentSlot,
        def: &'a Instrument,
        side: OrderSide,
        order_type: OrderType,
        quantity: Quantity,
        price: Option<Price>,
        reduce_only: bool,
        replaced: Option<Quantity>,
        instruments: &InstrumentTable,
    ) -> OrderCheck<'a> {
        let mut check =
            OrderCheck::new(def, CommandKind::Open, side, order_type, quantity, price, now.ts);
        check.slot = slot.0;
        check.strategy = s.0;
        check.reduce_only = reduce_only;
        check.position_raw = self.portfolio.venue(slot).map_or(0, NettingPosition::signed_raw);
        check.open = self.oms.open_quantity(slot.0, None);
        check.reference = self.portfolio.valuation(slot);
        if let Some(replaced) = replaced {
            check.kind = if quantity.raw() > replaced.raw() {
                CommandKind::ModifyUp
            } else {
                CommandKind::Modify
            };
            return check;
        }
        check.kind = classify(side, quantity.raw(), check.position_raw);
        let at = price.or(check.reference);
        if self.risk.config().check_margin && check.kind == CommandKind::Open {
            if let (Some(at), Some(available)) =
                (at, self.available(def.settlement_currency(), instruments))
            {
                if let Ok(required) = self.portfolio.order_margin(def, at, quantity) {
                    check.margin = Some(MarginCheck {
                        available_raw: available,
                        required_raw: required.raw(),
                    });
                }
            }
        }
        check
    }
}

impl State for Trading {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.oms.write(w);
        w.u64(self.ids.next_seq());
        self.strategy_ids.write(w);
        w.u32(self.events.len() as u32);
        self.portfolio.write(w);
        self.risk.write(w);
        self.stats.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.oms.read(r);
        let next_seq = r.u64();
        if next_seq == 0 || self.ids.resume_after(next_seq - 1).is_err() {
            r.fail(Status::InvalidState);
        }
        let n = self.strategy_ids.len();
        self.strategy_ids.read(r);
        if self.strategy_ids.len() != n {
            r.fail(Status::InvalidState);
        }
        if r.u32() != 0 {
            r.fail(Status::InvalidState); // snapshots are taken between steps
        }
        self.portfolio.read(r);
        self.risk.read(r);
        self.stats.read(r);
    }
}
