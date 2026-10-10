//! Kernel layer `engine` (docs/architecture.md sections 3, 4.3 and 5): `step(key, event)` updates
//! the kernel state, calls strategies and collects outputs. It reads nothing but its arguments and
//! its state, so replaying the same inputs reproduces the same calls and outputs.
//!
//! Delivery order within a step is fixed: a trade goes to its subscribers first, then the
//! features it updates, then the bars it completes; within one (row, kind) subscribers are served
//! in subscription order. Subscribers are collected before any callback runs, so a callback that
//! subscribes or unsubscribes changes the next delivery, not the current one.
//!
//! Order events: a venue event is applied to the OMS (a fill is also booked in the portfolio) and
//! delivered to the order's strategy at once; position events and the events the kernel produces
//! for commands (`OrderSubmitted`, `OrderDenied`, pending update and cancel) are delivered after
//! the input's other callbacks, in the order they arose, including those arising from these
//! deliveries.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod lifecycle;

use alloc::boxed::Box;
use alloc::vec::Vec;

use data::bars::BarAggregator;
use data::intern::{BarKey, InstrumentSlot};
use data::subscription::{Cadence, DataKind, Decision, StrategyIndex};
use data::{FeatureId, FeatureValue};
use kernel_core::clock::{FiredTimer, TimerKey};
use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{DurationNanos, EventKey, FixedVec, Result, Status, UnixNanos};
use model::bar::Bar;
use model::data::{
    FundingRateUpdate, IndexPriceUpdate, InstrumentClose, InstrumentStatus, LiquidationOrder,
    MarkPriceUpdate, OrderBookDeltas, QuoteTick, TradeTick,
};
use model::enums::{NodeState, StopMode};
use model::event::{NodeLifecycle, Shutdown, TimerFired};
use model::instruments::Instrument;
use model::order_events::OrderEvent;
use model::outputs::{FeatureUpdate, Output};
use model::Event;
use risk::TradingTrigger;
use strategy::kernel::{Pending, PendingValue, KERNEL_TIMER_OWNER};
use strategy::trading::KernelEvent;
use strategy::{Context, ErrorPolicy, Kernel, KernelConfig, Strategy, StrategyFailure};

pub use strategy;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EngineStats {
    pub steps: u64,
    /// Strategy callbacks made.
    pub callbacks: u64,
    /// Bars an aggregator completed but had no room to hand over.
    pub bars_overflowed: u64,
    /// `Conflated` updates that found the pending buffer full.
    pub pending_dropped: u64,
}
kernel_core::state_fields!(EngineStats { steps, callbacks, bars_overflowed, pending_dropped });

/// What one delivery carries.
#[derive(Clone, Copy, Debug)]
enum Payload<'a> {
    Trade(&'a TradeTick),
    Quote(&'a QuoteTick),
    /// The book of the slot (after a quote fed an L1 book).
    Book(InstrumentSlot),
    /// Deltas, then the book they updated.
    Deltas(&'a OrderBookDeltas, InstrumentSlot),
    Bar(&'a Bar),
    MarkPrice(&'a MarkPriceUpdate),
    IndexPrice(&'a IndexPriceUpdate),
    FundingRate(&'a FundingRateUpdate),
    InstrumentStatus(&'a InstrumentStatus),
    InstrumentClose(&'a InstrumentClose),
    Liquidation(&'a LiquidationOrder),
    Instrument(&'a Instrument),
    Feature {
        id: FeatureId,
        value: FeatureValue,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    },
}

impl Payload<'_> {
    /// What a `Conflated` subscriber keeps of it; for deltas the book itself (every delta matters).
    fn pending(&self) -> PendingValue {
        match *self {
            Payload::Trade(v) => PendingValue::Trade(*v),
            Payload::Quote(v) => PendingValue::Quote(*v),
            Payload::Book(slot) | Payload::Deltas(_, slot) => PendingValue::Book(slot),
            Payload::Bar(v) => PendingValue::Bar(*v),
            Payload::MarkPrice(v) => PendingValue::MarkPrice(*v),
            Payload::IndexPrice(v) => PendingValue::IndexPrice(*v),
            Payload::FundingRate(v) => PendingValue::FundingRate(*v),
            Payload::InstrumentStatus(v) => PendingValue::InstrumentStatus(*v),
            Payload::InstrumentClose(v) => PendingValue::InstrumentClose(*v),
            Payload::Liquidation(v) => PendingValue::Liquidation(*v),
            Payload::Instrument(v) => PendingValue::Instrument(*v),
            Payload::Feature { id, value, ts_event, ts_init } => {
                PendingValue::Feature { id, value, ts_event, ts_init }
            }
        }
    }
}

/// The deterministic kernel: state, strategies, `step`.
pub struct Engine {
    kernel: Kernel,
    strategies: Vec<Box<dyn Strategy>>,
    policy: ErrorPolicy,
    calls: FixedVec<StrategyIndex>,
    stats: EngineStats,
}

impl core::fmt::Debug for Engine {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Engine")
            .field("strategies", &self.strategies.len())
            .field("stats", &self.stats)
            .finish_non_exhaustive()
    }
}

impl Engine {
    /// `InvalidArgument` when there are more strategies than the configuration allows.
    pub fn new(
        config: KernelConfig,
        strategies: Vec<Box<dyn Strategy>>,
        policy: ErrorPolicy,
    ) -> Result<Self> {
        if strategies.len() > config.strategies as usize {
            return Err(Status::InvalidArgument);
        }
        Ok(Self {
            kernel: Kernel::new(config),
            strategies,
            policy,
            calls: FixedVec::with_capacity(config.strategies as usize),
            stats: EngineStats::default(),
        })
    }

    #[must_use]
    pub const fn kernel(&self) -> &Kernel {
        &self.kernel
    }
    pub fn kernel_mut(&mut self) -> &mut Kernel {
        &mut self.kernel
    }
    #[must_use]
    pub const fn stats(&self) -> &EngineStats {
        &self.stats
    }
    #[must_use]
    pub fn strategy_count(&self) -> usize {
        self.strategies.len()
    }
    #[must_use]
    pub fn strategy(&self, s: StrategyIndex) -> Option<&dyn Strategy> {
        self.strategies.get(s.0 as usize).map(AsRef::as_ref)
    }

    /// The outputs of the last step, in the order they arose.
    #[must_use]
    pub fn outputs(&self) -> &[Output] {
        &self.kernel.outputs
    }
    /// Callback failures of the last step.
    #[must_use]
    pub fn failures(&self) -> &[StrategyFailure] {
        &self.kernel.failures
    }
    /// The next timer due, without firing it; the node turns it into a `TimerFired` input.
    #[must_use]
    pub fn next_timer(&self) -> Option<FiredTimer> {
        self.kernel.timers.peek()
    }
    #[must_use]
    pub const fn halt_requested(&self) -> bool {
        self.kernel.halt_requested
    }
    #[must_use]
    pub const fn stop_requested(&self) -> bool {
        self.kernel.stop_requested
    }
    /// Orders not yet in a terminal state.
    #[must_use]
    pub fn open_orders(&self) -> u32 {
        self.kernel.trading.oms.open_orders()
    }

    // ---- step ---------------------------------------------------------------------------------

    /// Processes one input event. The outputs and failures of the previous step are cleared
    /// first.
    pub fn step(&mut self, key: EventKey, event: &Event) -> Result<()> {
        self.kernel.current = key;
        self.kernel.outputs.clear();
        self.kernel.failures.clear();
        self.kernel.trading.begin_step();
        self.stats.steps += 1;
        let result = self.dispatch(event);
        self.deliver_kernel_events();
        self.kernel.timers.prune();
        result
    }

    fn dispatch(&mut self, event: &Event) -> Result<()> {
        match event {
            Event::TradeTick(e) => self.on_trade(e),
            Event::QuoteTick(e) => self.on_quote(e),
            Event::OrderBookDeltas(e) => self.on_deltas(e),
            Event::Bar(e) => {
                let key = BarKey(self.kernel.bars.intern(e.bar_type)?);
                self.deliver(key.0, DataKind::Bar, e.ts_init, Payload::Bar(e));
                Ok(())
            }
            Event::MarkPriceUpdate(e) => {
                let slot = self.kernel.instruments.intern(e.instrument_id)?;
                self.kernel.trading.portfolio.on_mark(slot, e.value)?;
                self.deliver(slot.0, DataKind::MarkPrice, e.ts_init, Payload::MarkPrice(e));
                Ok(())
            }
            Event::IndexPriceUpdate(e) => {
                let slot = self.kernel.instruments.intern(e.instrument_id)?;
                self.deliver(slot.0, DataKind::IndexPrice, e.ts_init, Payload::IndexPrice(e));
                Ok(())
            }
            Event::FundingRateUpdate(e) => {
                let slot = self.kernel.instruments.intern(e.instrument_id)?;
                let k = &mut self.kernel;
                // Funding settles before subscribers see the update.
                k.trading.on_funding(k.current, slot, e, &k.instruments)?;
                self.deliver(slot.0, DataKind::FundingRate, e.ts_init, Payload::FundingRate(e));
                Ok(())
            }
            Event::InstrumentStatus(e) => {
                let slot = self.kernel.instruments.intern(e.instrument_id)?;
                self.kernel.trading.risk.on_status(slot.0, e);
                self.deliver(
                    slot.0,
                    DataKind::InstrumentStatus,
                    e.ts_init,
                    Payload::InstrumentStatus(e),
                );
                Ok(())
            }
            Event::InstrumentClose(e) => {
                let slot = self.kernel.instruments.intern(e.instrument_id)?;
                self.deliver(
                    slot.0,
                    DataKind::InstrumentClose,
                    e.ts_init,
                    Payload::InstrumentClose(e),
                );
                Ok(())
            }
            Event::LiquidationOrder(e) => {
                let slot = self.kernel.instruments.intern(e.instrument_id)?;
                self.deliver(slot.0, DataKind::Liquidation, e.ts_init, Payload::Liquidation(e));
                Ok(())
            }
            Event::Instrument(e) => {
                let slot = self.kernel.define_instrument(*e)?;
                self.deliver(slot.0, DataKind::Instrument, e.ts_init, Payload::Instrument(e));
                Ok(())
            }
            Event::Order(e) => self.on_venue_order_event(e),
            Event::AccountState(e) => self.kernel.trading.portfolio.set_account(e),
            Event::RateLimitFeedback(e) => {
                let limiter = self.kernel.trading.risk.limiter();
                limiter.feedback(e.ts_init, 10_000_000_000, e.order_count_10s);
                limiter.feedback(e.ts_init, 60_000_000_000, e.order_count_1m);
                Ok(())
            }
            Event::TimerFired(e) => self.on_timer_fired(e),
            Event::BatchEnd(_) => {
                self.flush_batch();
                Ok(())
            }
            Event::NodeLifecycle(e) => {
                self.on_lifecycle(e);
                Ok(())
            }
            Event::Shutdown(e) => self.on_shutdown(*e),
        }
    }

    // ---- market data --------------------------------------------------------------------------

    fn on_trade(&mut self, t: &TradeTick) -> Result<()> {
        let slot = self.kernel.instruments.intern(t.instrument_id)?;
        self.kernel.trading.portfolio.on_trade_price(slot, t.price)?;
        self.deliver(slot.0, DataKind::Trade, t.ts_init, Payload::Trade(t));
        self.feed_features(slot, Some(t), None)?;
        self.feed_aggregators(slot, Some(t), None)
    }

    fn on_quote(&mut self, q: &QuoteTick) -> Result<()> {
        let slot = self.kernel.instruments.intern(q.instrument_id)?;
        let mut l1 = false;
        if let Some(book) =
            self.kernel.book_for_update(slot, q.bid_price, q.bid_size.precision())?
        {
            if book.book_type() != Some(model::enums::BookType::L2Mbp) {
                book.apply_quote(q)?;
                l1 = true;
            }
        }
        self.deliver(slot.0, DataKind::Quote, q.ts_init, Payload::Quote(q));
        if l1 {
            self.deliver(slot.0, DataKind::Book, q.ts_init, Payload::Book(slot));
        }
        self.feed_features(slot, None, Some(q))?;
        self.feed_aggregators(slot, None, Some(q))
    }

    fn on_deltas(&mut self, d: &OrderBookDeltas) -> Result<()> {
        let slot = self.kernel.instruments.intern(d.instrument_id)?;
        let Some(first) = d.deltas.first() else { return Ok(()) };
        if let Some(book) =
            self.kernel.book_for_update(slot, first.order.price, first.order.size.precision())?
        {
            if book.book_type() != Some(model::enums::BookType::L1Mbp) {
                book.apply_deltas(d)?;
            }
        }
        self.deliver(slot.0, DataKind::Book, d.ts_init, Payload::Deltas(d, slot));
        Ok(())
    }

    fn feed_features(
        &mut self,
        slot: InstrumentSlot,
        t: Option<&TradeTick>,
        q: Option<&QuoteTick>,
    ) -> Result<()> {
        // Collect first: delivering a feature runs callbacks, which must not hold the graph.
        let mut produced: FixedVec<(FeatureId, FeatureValue)> =
            FixedVec::with_capacity(self.kernel.features.len());
        let (ts_event, ts_init) = match (t, q) {
            (Some(t), _) => {
                self.kernel.features.on_trade(slot, t, |id, v| {
                    let _ = produced.push((id, v));
                })?;
                (t.ts_event, t.ts_init)
            }
            (None, Some(q)) => {
                self.kernel.features.on_quote(slot, q, |id, v| {
                    let _ = produced.push((id, v));
                })?;
                (q.ts_event, q.ts_init)
            }
            (None, None) => return Ok(()),
        };
        for &(id, value) in &produced {
            self.deliver(
                id.0,
                DataKind::Feature,
                ts_init,
                Payload::Feature { id, value, ts_event, ts_init },
            );
        }
        Ok(())
    }

    /// Creates each aggregator on its first update (the precisions come from the data), feeds
    /// it, and delivers the bars it completes.
    fn feed_aggregators(
        &mut self,
        slot: InstrumentSlot,
        t: Option<&TradeTick>,
        q: Option<&QuoteTick>,
    ) -> Result<()> {
        for i in 0..self.kernel.aggregators.len() {
            let a = &mut self.kernel.aggregators[i];
            if a.slot != slot {
                continue;
            }
            if a.aggregator.is_none() {
                let (pp, sp) = match (t, q) {
                    (Some(t), _) => (t.price.precision(), t.size.precision()),
                    (None, Some(q)) => (q.bid_price.precision(), q.bid_size.precision()),
                    (None, None) => return Ok(()),
                };
                a.aggregator = Some(BarAggregator::new(a.bar_type, pp, sp)?);
            }
            let Some(agg) = a.aggregator.as_mut() else { continue };
            let completed = match (t, q) {
                (Some(t), _) if agg.uses_trades() => agg.on_trade(t)?,
                (None, Some(q)) if !agg.uses_trades() => agg.on_quote(q)?,
                _ => continue,
            };
            self.stats.bars_overflowed += u64::from(completed.overflow);
            let key = a.key;
            for bar in completed.iter() {
                self.deliver(key.0, DataKind::Bar, bar.ts_init, Payload::Bar(bar));
            }
            self.arm_close_timer(i)?;
        }
        Ok(())
    }

    fn arm_close_timer(&mut self, index: usize) -> Result<()> {
        let a = &mut self.kernel.aggregators[index];
        let close = a.aggregator.as_ref().and_then(BarAggregator::next_close);
        if close == a.armed_deadline {
            return Ok(());
        }
        if let Some(handle) = a.timer.take() {
            let _ = self.kernel.timers.cancel(handle);
        }
        a.armed_deadline = close;
        if let Some(close) = close {
            let key = TimerKey::new(KERNEL_TIMER_OWNER, index as u32);
            let handle = self.kernel.timers.schedule(close, DurationNanos::default(), key)?;
            self.kernel.aggregators[index].timer = Some(handle);
        }
        Ok(())
    }

    // ---- delivery -----------------------------------------------------------------------------

    fn deliver(&mut self, row: u32, kind: DataKind, ts: UnixNanos, payload: Payload<'_>) {
        self.calls.clear();
        let mut overflowing: FixedVec<usize> = FixedVec::with_capacity(0);
        for i in 0..self.kernel.matrix.subscribers(row, kind).len() {
            let (strategy, cadence) = {
                let sub = &self.kernel.matrix.subscribers(row, kind)[i];
                (sub.strategy, sub.cadence)
            };
            if self.kernel.is_disabled(strategy) {
                continue;
            }
            let decision = self.kernel.matrix.subscribers_mut(row, kind)[i].on_update(ts);
            match decision {
                Decision::Now => {
                    let _ = self.calls.push(strategy);
                }
                Decision::Skip => {}
                Decision::Defer => match cadence {
                    Cadence::Conflated => self.store_pending(strategy, row, kind, &payload),
                    Cadence::OnBatch => {
                        if let Some(index) = self.append_batch(strategy, row, kind, &payload) {
                            if overflowing.capacity() == 0 {
                                overflowing = FixedVec::with_capacity(self.kernel.batches.len());
                            }
                            let _ = overflowing.push(index);
                        }
                    }
                    Cadence::Every | Cadence::Sampled { .. } => {}
                },
            }
        }
        if let Payload::Feature { id, value, ts_event, ts_init } = payload {
            if !self.calls.is_empty() {
                let _ = self.kernel.outputs.push(Output::FeatureUpdate(FeatureUpdate {
                    feature_id: id.0,
                    value: value.0,
                    ts_event,
                    ts_init,
                }));
            }
        }
        for i in 0..self.calls.len() {
            let s = self.calls[i];
            self.call(s, &payload);
        }
        // A full buffer is delivered early, deterministically, then takes the update.
        for &index in &overflowing {
            self.flush_batch_buffer(index);
            let (strategy, row, kind) = {
                let b = &self.kernel.batches[index];
                (b.strategy, b.slot.0, b.kind)
            };
            let _ = self.append_batch(strategy, row, kind, &payload);
        }
    }

    fn store_pending(
        &mut self,
        strategy: StrategyIndex,
        row: u32,
        kind: DataKind,
        payload: &Payload<'_>,
    ) {
        let value = payload.pending();
        if let Some(p) = self
            .kernel
            .pending
            .iter_mut()
            .find(|p| p.strategy == strategy && p.row == row && p.kind == kind)
        {
            p.value = value;
            return;
        }
        if self.kernel.pending.push(Pending { strategy, row, kind, value }).is_err() {
            self.stats.pending_dropped += 1;
        }
    }

    /// Appends to the strategy's batch buffer; the buffer's index when it was full.
    fn append_batch(
        &mut self,
        strategy: StrategyIndex,
        row: u32,
        kind: DataKind,
        payload: &Payload<'_>,
    ) -> Option<usize> {
        let index = self
            .kernel
            .batches
            .iter()
            .position(|b| b.strategy == strategy && b.slot.0 == row && b.kind == kind)?;
        let b = &mut self.kernel.batches[index];
        let pushed = match payload {
            Payload::Trade(t) => b.trades.push(**t),
            Payload::Quote(q) => b.quotes.push(**q),
            _ => return None,
        };
        pushed.is_err().then_some(index)
    }

    fn flush_batch_buffer(&mut self, index: usize) {
        let (strategy, slot, kind) = {
            let b = &self.kernel.batches[index];
            (b.strategy, b.slot, b.kind)
        };
        let Some(id) = self.kernel.instruments.id_of(slot).copied() else { return };
        if self.kernel.is_disabled(strategy) {
            self.kernel.batches[index].trades.clear();
            self.kernel.batches[index].quotes.clear();
            return;
        }
        // The buffer is on loan to the callback; a new buffer takes updates meanwhile.
        let mut trades =
            core::mem::replace(&mut self.kernel.batches[index].trades, FixedVec::with_capacity(0));
        let mut quotes =
            core::mem::replace(&mut self.kernel.batches[index].quotes, FixedVec::with_capacity(0));
        let result = {
            let Some(strategy_ref) = self.strategies.get_mut(strategy.0 as usize) else { return };
            let mut ctx = Context::new(&mut self.kernel, strategy);
            self.stats.callbacks += 1;
            match kind {
                DataKind::Trade if !trades.is_empty() => {
                    strategy_ref.on_trade_batch(&mut ctx, &id, &trades)
                }
                DataKind::Quote if !quotes.is_empty() => {
                    strategy_ref.on_quote_batch(&mut ctx, &id, &quotes)
                }
                _ => Ok(()),
            }
        };
        trades.clear();
        quotes.clear();
        self.kernel.batches[index].trades = trades;
        self.kernel.batches[index].quotes = quotes;
        self.note(strategy, result);
    }

    /// `BatchEnd`: conflated updates in the order they became pending, then the `OnBatch`
    /// buffers in subscription order.
    fn flush_batch(&mut self) {
        for i in 0..self.kernel.pending.len() {
            let p = self.kernel.pending[i];
            if self.kernel.is_disabled(p.strategy) {
                continue;
            }
            match p.value {
                PendingValue::Trade(v) => self.call(p.strategy, &Payload::Trade(&v)),
                PendingValue::Quote(v) => self.call(p.strategy, &Payload::Quote(&v)),
                PendingValue::Book(slot) => self.call(p.strategy, &Payload::Book(slot)),
                PendingValue::Bar(v) => self.call(p.strategy, &Payload::Bar(&v)),
                PendingValue::MarkPrice(v) => self.call(p.strategy, &Payload::MarkPrice(&v)),
                PendingValue::IndexPrice(v) => self.call(p.strategy, &Payload::IndexPrice(&v)),
                PendingValue::FundingRate(v) => self.call(p.strategy, &Payload::FundingRate(&v)),
                PendingValue::InstrumentStatus(v) => {
                    self.call(p.strategy, &Payload::InstrumentStatus(&v));
                }
                PendingValue::InstrumentClose(v) => {
                    self.call(p.strategy, &Payload::InstrumentClose(&v));
                }
                PendingValue::Liquidation(v) => self.call(p.strategy, &Payload::Liquidation(&v)),
                PendingValue::Instrument(v) => self.call(p.strategy, &Payload::Instrument(&v)),
                PendingValue::Feature { id, value, ts_event, ts_init } => {
                    let _ = self.kernel.outputs.push(Output::FeatureUpdate(FeatureUpdate {
                        feature_id: id.0,
                        value: value.0,
                        ts_event,
                        ts_init,
                    }));
                    self.call(p.strategy, &Payload::Feature { id, value, ts_event, ts_init });
                }
            }
        }
        self.kernel.pending.clear();
        for i in 0..self.kernel.batches.len() {
            self.flush_batch_buffer(i);
        }
        for (_, _, sub) in self.kernel.matrix.iter_mut() {
            let _ = sub.on_batch_end();
        }
    }

    fn call(&mut self, s: StrategyIndex, payload: &Payload<'_>) {
        let Some(strategy) = self.strategies.get_mut(s.0 as usize) else { return };
        self.stats.callbacks += 1;
        let result = match *payload {
            Payload::Trade(v) => strategy.on_trade(&mut Context::new(&mut self.kernel, s), v),
            Payload::Quote(v) => strategy.on_quote(&mut Context::new(&mut self.kernel, s), v),
            Payload::Bar(v) => strategy.on_bar(&mut Context::new(&mut self.kernel, s), v),
            Payload::MarkPrice(v) => {
                strategy.on_mark_price(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::IndexPrice(v) => {
                strategy.on_index_price(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::FundingRate(v) => {
                strategy.on_funding_rate(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::InstrumentStatus(v) => {
                strategy.on_instrument_status(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::InstrumentClose(v) => {
                strategy.on_instrument_close(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::Liquidation(v) => {
                strategy.on_liquidation(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::Instrument(v) => {
                strategy.on_instrument(&mut Context::new(&mut self.kernel, s), v)
            }
            Payload::Feature { id, value, ts_event, ts_init } => strategy.on_feature(
                &mut Context::new(&mut self.kernel, s),
                id,
                value,
                ts_event,
                ts_init,
            ),
            Payload::Deltas(d, slot) => {
                let first = strategy.on_book_deltas(&mut Context::new(&mut self.kernel, s), d);
                if first.is_err() {
                    first
                } else {
                    Self::call_book(&mut self.kernel, strategy.as_mut(), s, slot)
                }
            }
            Payload::Book(slot) => Self::call_book(&mut self.kernel, strategy.as_mut(), s, slot),
        };
        self.note(s, result);
    }

    /// The book is on loan to the callback (`ctx.book` of that instrument is `None` meanwhile).
    fn call_book(
        kernel: &mut Kernel,
        strategy: &mut dyn Strategy,
        s: StrategyIndex,
        slot: InstrumentSlot,
    ) -> Result<()> {
        let Some(id) = kernel.instruments.id_of(slot).copied() else { return Ok(()) };
        let Some(book) = kernel.books.get_mut(slot.index()).and_then(Option::take) else {
            return Ok(());
        };
        let result = strategy.on_book(&mut Context::new(kernel, s), &id, &book.view());
        kernel.books[slot.index()] = Some(book);
        result
    }

    /// Applies the error policy to a callback's result.
    fn note(&mut self, s: StrategyIndex, result: Result<()>) {
        let Err(status) = result else { return };
        self.kernel.fail(s, status);
        match self.policy {
            ErrorPolicy::Ignore => {}
            ErrorPolicy::HaltStrategy | ErrorPolicy::HaltNode => {
                if let Some(d) = self.kernel.disabled.get_mut(s.0 as usize) {
                    *d = true;
                }
                let k = &mut self.kernel;
                let _ = k.trading.cancel_all(k.current, s, None, &mut k.outputs);
                if self.policy == ErrorPolicy::HaltNode {
                    self.kernel.halt_requested = true;
                }
            }
        }
    }

    // ---- orders -------------------------------------------------------------------------------

    fn on_venue_order_event(&mut self, e: &OrderEvent) -> Result<()> {
        let k = &mut self.kernel;
        let Some(owner) = k.trading.on_venue_event(k.current, e, &k.instruments)? else {
            return Ok(()); // refused, stale, duplicate and unknown events are counted, not fatal
        };
        self.deliver_order_event(owner, e);
        Ok(())
    }

    fn deliver_order_event(&mut self, s: StrategyIndex, e: &OrderEvent) {
        if self.kernel.is_disabled(s) {
            return;
        }
        let Some(strategy) = self.strategies.get_mut(s.0 as usize) else { return };
        self.stats.callbacks += 1;
        let result = strategy.on_order_event(&mut Context::new(&mut self.kernel, s), e);
        self.note(s, result);
    }

    /// The queue has fixed capacity and never reallocates, so entries stay put while callbacks
    /// append to it.
    fn deliver_kernel_events(&mut self) {
        let mut i = 0;
        while i < self.kernel.trading.events.len() {
            let p = self.kernel.trading.events[i];
            i += 1;
            if self.kernel.is_disabled(p.strategy) {
                continue;
            }
            let Some(strategy) = self.strategies.get_mut(p.strategy.0 as usize) else { continue };
            self.stats.callbacks += 1;
            let result = match &p.event {
                KernelEvent::Order(e) => {
                    strategy.on_order_event(&mut Context::new(&mut self.kernel, p.strategy), e)
                }
                KernelEvent::Position(e) => {
                    strategy.on_position_event(&mut Context::new(&mut self.kernel, p.strategy), e)
                }
            };
            self.note(p.strategy, result);
        }
        self.kernel.trading.events.clear();
    }

    // ---- lifecycle, timers, control -----------------------------------------------------------

    fn on_timer_fired(&mut self, e: &TimerFired) -> Result<()> {
        // The queue's own copy fires too (it may have been re-armed when periodic).
        while let Some(fired) = self.kernel.timers.pop_due(e.deadline) {
            if fired.key == e.key {
                break;
            }
        }
        self.kernel.timer_fired(e.key);
        if e.key.owner == KERNEL_TIMER_OWNER {
            return self.on_bar_close(e.key.id as usize, e.deadline);
        }
        let s = StrategyIndex(e.key.owner as u16);
        if self.kernel.is_disabled(s) {
            return Ok(());
        }
        let Some(strategy) = self.strategies.get_mut(s.0 as usize) else { return Ok(()) };
        self.stats.callbacks += 1;
        let result = strategy.on_timer(&mut Context::new(&mut self.kernel, s), e.key, e.deadline);
        self.note(s, result);
        Ok(())
    }

    fn on_bar_close(&mut self, index: usize, deadline: UnixNanos) -> Result<()> {
        let Some(a) = self.kernel.aggregators.get_mut(index) else { return Ok(()) };
        a.timer = None;
        a.armed_deadline = None;
        let key = a.key;
        let bar = a.aggregator.as_mut().and_then(|agg| agg.on_time(deadline));
        if let Some(bar) = bar {
            self.deliver(key.0, DataKind::Bar, bar.ts_init, Payload::Bar(&bar));
        }
        self.arm_close_timer(index)
    }

    fn on_lifecycle(&mut self, e: &NodeLifecycle) {
        self.kernel.node_state = e.to;
        self.kernel.trading.on_lifecycle(e.from, e.to);
        if e.to == NodeState::Running && !self.kernel.started {
            self.kernel.started = true;
            self.for_each_active(|strategy, ctx| strategy.on_start(ctx));
        } else if e.to == NodeState::Stopping && !self.kernel.stopped {
            if self.kernel.started {
                self.flush_batch();
                self.for_each_active(|strategy, ctx| strategy.on_stop(ctx));
            }
            self.kernel.stopped = true;
        }
    }

    fn on_shutdown(&mut self, e: Shutdown) -> Result<()> {
        self.kernel.stop_requested = true;
        match e.mode {
            StopMode::LeaveOrders => {}
            StopMode::CancelAllThenExit => {
                let k = &mut self.kernel;
                k.trading.kill_switch(k.current, &mut k.outputs)?;
            }
            StopMode::KillSwitch => {
                let k = &mut self.kernel;
                k.trading.risk.apply(TradingTrigger::AdminHalt);
                k.trading.kill_switch(k.current, &mut k.outputs)?;
            }
        }
        Ok(())
    }

    fn for_each_active(
        &mut self,
        mut call: impl FnMut(&mut dyn Strategy, &mut Context<'_>) -> Result<()>,
    ) {
        for i in 0..self.strategies.len() {
            let s = StrategyIndex(i as u16);
            if self.kernel.is_disabled(s) {
                continue;
            }
            self.stats.callbacks += 1;
            let result = call(self.strategies[i].as_mut(), &mut Context::new(&mut self.kernel, s));
            self.note(s, result);
        }
    }

    // ---- snapshots (docs/architecture.md section 16.3) ----------------------------------------

    /// Whether every strategy describes its own state, so a restored snapshot continues exactly
    /// as this engine would.
    #[must_use]
    pub fn snapshot_complete(&self) -> bool {
        self.strategies.iter().all(|s| s.has_state())
    }

    /// The kernel's state and each strategy's own. Between steps only.
    pub fn save_state(&self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        let mut w = StateWriter::new(&mut out);
        self.kernel.write(&mut w);
        self.stats.write(&mut w);
        w.u16(self.strategies.len() as u16);
        for s in &self.strategies {
            w.u8(u8::from(s.has_state()));
            if s.has_state() {
                let mut inner = Vec::new();
                let mut iw = StateWriter::new(&mut inner);
                s.save_state(&mut iw);
                iw.status()?;
                w.u32(inner.len() as u32);
                w.raw(&inner);
            }
        }
        w.status()?;
        Ok(out)
    }

    /// Restores what `save_state` wrote into this engine, built from the same configuration and
    /// the same strategies. On failure the engine is unusable and must be discarded.
    pub fn load_state(&mut self, input: &[u8]) -> Result<()> {
        let mut r = StateReader::new(input);
        self.kernel.read(&mut r);
        self.stats.read(&mut r);
        let count = r.u16();
        r.status()?;
        if count as usize != self.strategies.len() {
            return Err(Status::InvalidArgument);
        }
        for s in &mut self.strategies {
            let has = r.u8() == 1;
            r.status()?;
            if has != s.has_state() {
                return Err(Status::InvalidState);
            }
            if !has {
                continue;
            }
            let n = r.u32() as usize;
            let bytes = r.raw(n);
            r.status()?;
            let mut ir = StateReader::new(bytes);
            s.load_state(&mut ir);
            ir.status()?;
            if ir.remaining() != 0 {
                return Err(Status::InvalidArgument);
            }
        }
        if r.remaining() != 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}
