//! State the engine and the strategies' [`Context`](crate::Context) share (docs/architecture.md
//! sections 7.2-7.5 and 9): instruments, subscriptions, books, bar aggregators, features, timers,
//! orders and the outputs of the current step. The engine is the only writer outside `Context`
//! calls; everything is sized from [`KernelConfig`] at construction. Books and bar aggregators are
//! created on first use (subscription or first update) and never after the warm-up.

use data::bars::BarAggregator;
use data::book::{BookConfig, OrderBook};
use data::intern::{BarKey, BarTable, InstrumentSlot, InstrumentTable};
use data::subscription::{Cadence, DataKind, StrategyIndex, SubscriptionMatrix};
use data::{FeatureGraph, FeatureId, FeatureKind, FeatureSpec, FeatureValue};
use kernel_core::clock::{TimerHandle, TimerKey, TimerQueue};
use kernel_core::rng::CounterRng;
use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{DurationNanos, EventKey, FixedVec, Result, Status, UnixNanos};
use model::bar::{Bar, BarType};
use model::data::{
    FundingRateUpdate, IndexPriceUpdate, InstrumentClose, InstrumentStatus, LiquidationOrder,
    MarkPriceUpdate, QuoteTick, TradeTick,
};
use model::enums::{AggregationSource, NodeState};
use model::instruments::Instrument;
use model::outputs::{Output, RecordTag, StrategyRecord};
use model::wire::{Wire, WireWriter};
use model::{InstrumentId, Price};

use crate::trading::{Trading, TradingConfig};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KernelConfig {
    pub instruments: u32,
    pub strategies: u16,
    pub timers: u32,
    /// Rows per `OnBatch` buffer.
    pub batch: u32,
    pub features: u32,
    pub bar_types: u32,
    /// `Conflated` and `OnBatch` subscriptions.
    pub buffers: u32,
    /// Outputs per step.
    pub outputs: u32,
    pub book_window_levels: u32,
    pub book_overflow_levels: u32,
    pub seed: u64,
    pub trading: TradingConfig,
}

impl Default for KernelConfig {
    fn default() -> Self {
        Self {
            instruments: 64,
            strategies: 8,
            timers: 256,
            batch: 1024,
            features: 64,
            bar_types: 64,
            buffers: 1024,
            outputs: 4096,
            book_window_levels: 16384,
            book_overflow_levels: 4096,
            seed: 0,
            trading: TradingConfig::default(),
        }
    }
}

/// What happens to a strategy whose callback failed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ErrorPolicy {
    /// The strategy receives no more callbacks and its open orders are canceled.
    #[default]
    HaltStrategy,
    /// The node stops.
    HaltNode,
    Ignore,
}

/// A callback failure noticed during a step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StrategyFailure {
    pub strategy: StrategyIndex,
    pub status: Status,
}

/// Timers owned by the kernel itself (time-bar closes, by aggregator index) use this owner.
pub const KERNEL_TIMER_OWNER: u32 = u32::MAX;

/// A bar type a strategy subscribed to; INTERNAL ones aggregate here.
#[derive(Clone, Debug)]
pub struct AggregatorState {
    pub bar_type: BarType,
    pub slot: InstrumentSlot,
    pub key: BarKey,
    /// Created on the first update, when the precisions are known.
    pub aggregator: Option<BarAggregator>,
    pub armed_deadline: Option<UnixNanos>,
    pub timer: Option<TimerHandle>,
}

impl State for AggregatorState {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.bar_type.write(w);
        self.slot.write(w);
        self.key.write(w);
        match &self.aggregator {
            None => w.u8(0),
            Some(a) => {
                w.u8(1);
                w.u8(a.price_precision());
                w.u8(a.size_precision());
                a.write(w);
            }
        }
        self.armed_deadline.write(w);
        self.timer.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.bar_type.read(r);
        self.slot.read(r);
        self.key.read(r);
        self.aggregator = match r.u8() {
            0 => None,
            1 => {
                let (pp, sp) = (r.u8(), r.u8());
                match BarAggregator::new(self.bar_type, pp, sp) {
                    Ok(mut a) => {
                        a.read(r);
                        Some(a)
                    }
                    Err(e) => {
                        r.fail(e);
                        None
                    }
                }
            }
            _ => {
                r.fail(Status::InvalidArgument);
                None
            }
        };
        self.armed_deadline.read(r);
        self.timer.read(r);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TimerEntry {
    pub key: TimerKey,
    pub handle: TimerHandle,
    pub periodic: bool,
}
kernel_core::state_fields!(TimerEntry { key, handle, periodic });

/// The latest update of a `Conflated` subscription, waiting for `BatchEnd`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(clippy::large_enum_variant)] // an instrument definition is the large side; few are pending
pub enum PendingValue {
    Trade(TradeTick),
    Quote(QuoteTick),
    /// The book of the slot, read at delivery.
    Book(InstrumentSlot),
    Bar(Bar),
    MarkPrice(MarkPriceUpdate),
    IndexPrice(IndexPriceUpdate),
    FundingRate(FundingRateUpdate),
    InstrumentStatus(InstrumentStatus),
    InstrumentClose(InstrumentClose),
    Liquidation(LiquidationOrder),
    Instrument(Instrument),
    Feature {
        id: FeatureId,
        value: FeatureValue,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    },
}

impl Default for PendingValue {
    fn default() -> Self {
        PendingValue::Book(InstrumentSlot(0))
    }
}

fn write_wire<T: Wire>(w: &mut StateWriter<'_>, v: &T) {
    let mut out = WireWriter::new();
    v.encode(&mut out);
    w.u32(out.len() as u32);
    w.raw(out.as_slice());
}
fn read_wire<T: Wire>(r: &mut StateReader<'_>) -> Option<T> {
    let n = r.u32() as usize;
    let bytes = r.raw(n);
    if !r.is_ok() {
        return None;
    }
    match T::from_wire(bytes) {
        Ok(v) => Some(v),
        Err(e) => {
            r.fail(e);
            None
        }
    }
}

impl State for PendingValue {
    fn write(&self, w: &mut StateWriter<'_>) {
        match self {
            PendingValue::Trade(v) => {
                w.u8(1);
                write_wire(w, v);
            }
            PendingValue::Quote(v) => {
                w.u8(2);
                write_wire(w, v);
            }
            PendingValue::Book(slot) => {
                w.u8(3);
                slot.write(w);
            }
            PendingValue::Bar(v) => {
                w.u8(4);
                write_wire(w, v);
            }
            PendingValue::MarkPrice(v) => {
                w.u8(5);
                write_wire(w, v);
            }
            PendingValue::IndexPrice(v) => {
                w.u8(6);
                write_wire(w, v);
            }
            PendingValue::FundingRate(v) => {
                w.u8(7);
                write_wire(w, v);
            }
            PendingValue::InstrumentStatus(v) => {
                w.u8(8);
                write_wire(w, v);
            }
            PendingValue::InstrumentClose(v) => {
                w.u8(9);
                write_wire(w, v);
            }
            PendingValue::Liquidation(v) => {
                w.u8(10);
                write_wire(w, v);
            }
            PendingValue::Instrument(v) => {
                w.u8(11);
                write_wire(w, v);
            }
            PendingValue::Feature { id, value, ts_event, ts_init } => {
                w.u8(12);
                w.u32(id.0);
                value.write(w);
                ts_event.write(w);
                ts_init.write(w);
            }
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let tag = r.u8();
        let read = match tag {
            1 => read_wire(r).map(PendingValue::Trade),
            2 => read_wire(r).map(PendingValue::Quote),
            3 => {
                let mut slot = InstrumentSlot(0);
                slot.read(r);
                Some(PendingValue::Book(slot))
            }
            4 => read_wire(r).map(PendingValue::Bar),
            5 => read_wire(r).map(PendingValue::MarkPrice),
            6 => read_wire(r).map(PendingValue::IndexPrice),
            7 => read_wire(r).map(PendingValue::FundingRate),
            8 => read_wire(r).map(PendingValue::InstrumentStatus),
            9 => read_wire(r).map(PendingValue::InstrumentClose),
            10 => read_wire(r).map(PendingValue::Liquidation),
            11 => read_wire(r).map(PendingValue::Instrument),
            12 => {
                let id = FeatureId(r.u32());
                let mut value = FeatureValue::default();
                value.read(r);
                let mut ts_event = UnixNanos::default();
                ts_event.read(r);
                let mut ts_init = UnixNanos::default();
                ts_init.read(r);
                Some(PendingValue::Feature { id, value, ts_event, ts_init })
            }
            _ => {
                r.fail(Status::InvalidArgument);
                None
            }
        };
        if let Some(v) = read {
            *self = v;
        }
    }
}

/// A `Conflated` delivery waiting for `BatchEnd`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Pending {
    pub strategy: StrategyIndex,
    pub row: u32,
    pub kind: DataKind,
    pub value: PendingValue,
}

impl Default for Pending {
    fn default() -> Self {
        Self {
            strategy: StrategyIndex(0),
            row: 0,
            kind: DataKind::Trade,
            value: PendingValue::default(),
        }
    }
}
kernel_core::state_fields!(Pending { strategy, row, kind, value });

/// An `OnBatch` subscription: every update of the batch.
#[derive(Clone, Debug)]
pub struct BatchBuffer {
    pub strategy: StrategyIndex,
    pub slot: InstrumentSlot,
    pub kind: DataKind,
    pub trades: FixedVec<TradeTick>,
    pub quotes: FixedVec<QuoteTick>,
}

impl State for BatchBuffer {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.strategy.write(w);
        self.slot.write(w);
        self.kind.write(w);
        w.u32(self.trades.len() as u32);
        for t in &self.trades {
            write_wire(w, t);
        }
        w.u32(self.quotes.len() as u32);
        for q in &self.quotes {
            write_wire(w, q);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.strategy.read(r);
        self.slot.read(r);
        self.kind.read(r);
        self.trades.clear();
        for _ in 0..r.u32() {
            if let Some(t) = read_wire(r) {
                r.check(self.trades.push(t));
            }
        }
        self.quotes.clear();
        for _ in 0..r.u32() {
            if let Some(q) = read_wire(r) {
                r.check(self.quotes.push(q));
            }
        }
    }
}

/// The kernel's services.
#[derive(Clone, Debug)]
#[allow(clippy::struct_excessive_bools)] // independent lifecycle flags, each snapshotted
pub struct Kernel {
    config: KernelConfig,
    pub current: EventKey,
    pub instruments: InstrumentTable,
    pub bars: BarTable,
    pub matrix: SubscriptionMatrix,
    pub features: FeatureGraph,
    pub books: FixedVec<Option<OrderBook>>,
    /// By slot: a book subscription exists.
    pub book_wanted: FixedVec<bool>,
    pub aggregators: FixedVec<AggregatorState>,
    pub timers: TimerQueue,
    pub timer_entries: FixedVec<TimerEntry>,
    pub pending: FixedVec<Pending>,
    pub batches: FixedVec<BatchBuffer>,
    pub outputs: FixedVec<Output>,
    pub failures: FixedVec<StrategyFailure>,
    pub disabled: FixedVec<bool>,
    pub trading: Trading,
    pub node_state: NodeState,
    pub started: bool,
    pub stopped: bool,
    pub stop_requested: bool,
    pub halt_requested: bool,
    rng: CounterRng,
}

impl Kernel {
    #[must_use]
    pub fn new(config: KernelConfig) -> Self {
        let rows = config.instruments.max(config.bar_types).max(config.features);
        let n = config.instruments as usize;
        Self {
            config,
            current: EventKey::default(),
            instruments: InstrumentTable::with_capacity(n),
            bars: BarTable::with_capacity(config.bar_types as usize),
            matrix: SubscriptionMatrix::new(rows, config.strategies),
            features: FeatureGraph::with_capacity(config.features as usize),
            books: FixedVec::from_iter_exact((0..n).map(|_| None)),
            book_wanted: FixedVec::from_iter_exact((0..n).map(|_| false)),
            aggregators: FixedVec::with_capacity(config.bar_types as usize),
            timers: TimerQueue::with_capacity(config.timers),
            timer_entries: FixedVec::with_capacity(config.timers as usize),
            pending: FixedVec::with_capacity(config.buffers as usize),
            batches: FixedVec::with_capacity(config.buffers as usize),
            outputs: FixedVec::with_capacity(config.outputs as usize),
            failures: FixedVec::with_capacity(config.strategies as usize),
            disabled: FixedVec::from_iter_exact((0..config.strategies).map(|_| false)),
            trading: Trading::new(
                &config.trading,
                config.instruments,
                config.strategies,
                config.seed,
            ),
            node_state: NodeState::Init,
            started: false,
            stopped: false,
            stop_requested: false,
            halt_requested: false,
            rng: CounterRng::new(config.seed),
        }
    }

    #[must_use]
    pub const fn config(&self) -> &KernelConfig {
        &self.config
    }

    // ---- time, randomness, records ------------------------------------------------------------

    #[must_use]
    pub const fn now(&self) -> UnixNanos {
        self.current.ts
    }

    /// A pure function of `(seed, input seq, strategy, key)`: replay draws the same numbers.
    #[must_use]
    pub const fn rng(&self, strategy: StrategyIndex, key: u32) -> u64 {
        self.rng.draw(self.current.seq, strategy.0 as u32, key)
    }

    pub fn record(&mut self, strategy: StrategyIndex, tag: &str, value: i64) -> Result<()> {
        let record = StrategyRecord {
            strategy_index: strategy.0,
            tag: RecordTag::from_text(tag)?,
            value,
            ts_init: self.current.ts,
        };
        self.outputs.push(Output::StrategyRecord(record))
    }

    // ---- timers -------------------------------------------------------------------------------

    /// Schedules (or reschedules) the owner's timer `id`. The deadline must not be in the past.
    pub fn set_timer(
        &mut self,
        key: TimerKey,
        deadline: UnixNanos,
        period: DurationNanos,
    ) -> Result<()> {
        if deadline < self.current.ts {
            return Err(Status::InvalidArgument);
        }
        let _ = self.cancel_timer(key);
        let handle = self.timers.schedule(deadline, period, key)?;
        self.timer_entries.push(TimerEntry { key, handle, periodic: !period.is_zero() })
    }

    pub fn cancel_timer(&mut self, key: TimerKey) -> Result<()> {
        let i = self.timer_entries.iter().position(|e| e.key == key).ok_or(Status::NotFound)?;
        let entry = self.timer_entries.remove_at(i).ok_or(Status::NotFound)?;
        let _ = self.timers.cancel(entry.handle);
        Ok(())
    }

    /// Forgets the entry of a one-shot timer that fired.
    pub fn timer_fired(&mut self, key: TimerKey) {
        if let Some(i) = self.timer_entries.iter().position(|e| e.key == key && !e.periodic) {
            let _ = self.timer_entries.remove_at(i);
        }
    }

    // ---- instruments and books ----------------------------------------------------------------

    /// An instrument definition (an input event): stored by slot. A redefinition replaces the
    /// earlier one.
    pub fn define_instrument(&mut self, definition: Instrument) -> Result<InstrumentSlot> {
        self.instruments.define(definition)
    }

    #[must_use]
    pub fn instrument(&self, id: &InstrumentId) -> Option<&Instrument> {
        self.instruments.instrument(self.instruments.slot_of(id)?)
    }

    /// The book of `slot`, created on first use; `None` when the instrument has no book
    /// subscription. `sample` gives the tick when no definition names it.
    pub fn book_for_update(
        &mut self,
        slot: InstrumentSlot,
        sample: Price,
        size_precision: u8,
    ) -> Result<Option<&mut OrderBook>> {
        if !self.book_wanted.get(slot.index()).copied().unwrap_or(false) {
            return Ok(None);
        }
        let entry = self.books.get_mut(slot.index()).ok_or(Status::OutOfRange)?;
        if entry.is_none() {
            let tick = match self.instruments.instrument(slot) {
                Some(def) => def.price_increment,
                None => Price::from_units(1, sample.precision())?,
            };
            *entry = Some(OrderBook::new(BookConfig {
                price_increment: tick,
                size_precision,
                window_levels: self.config.book_window_levels,
                overflow_levels: self.config.book_overflow_levels,
            })?);
        }
        Ok(entry.as_mut())
    }

    // ---- subscriptions ------------------------------------------------------------------------

    /// Instrument data by kind. `Book` needs a book, created on the first update; `OnBatch` is
    /// for trades and quotes only.
    pub fn subscribe(
        &mut self,
        strategy: StrategyIndex,
        kind: DataKind,
        id: &InstrumentId,
        cadence: Cadence,
    ) -> Result<()> {
        if matches!(kind, DataKind::Bar | DataKind::Feature) {
            return Err(Status::InvalidArgument); // subscribe_bars / declare_feature
        }
        let batchable = matches!(kind, DataKind::Trade | DataKind::Quote);
        if cadence == Cadence::OnBatch && !batchable {
            return Err(Status::InvalidArgument);
        }
        let slot = self.instruments.intern(*id)?;
        if kind == DataKind::Book {
            self.book_wanted[slot.index()] = true;
        }
        self.subscribe_row(strategy, slot.0, kind, cadence, Some(slot))
    }

    pub fn unsubscribe(
        &mut self,
        strategy: StrategyIndex,
        kind: DataKind,
        id: &InstrumentId,
    ) -> Result<()> {
        let slot = self.instruments.slot_of(id).ok_or(Status::NotFound)?;
        self.matrix.unsubscribe(slot.0, kind, strategy)?;
        self.batches.retain(|b| !(b.strategy == strategy && b.slot == slot && b.kind == kind));
        Ok(())
    }

    /// Bars of `bar_type`: EXTERNAL bars come from the data; INTERNAL bars are aggregated here.
    pub fn subscribe_bars(
        &mut self,
        strategy: StrategyIndex,
        bar_type: &BarType,
        cadence: Cadence,
    ) -> Result<()> {
        if cadence == Cadence::OnBatch {
            return Err(Status::InvalidArgument);
        }
        let key = BarKey(self.bars.intern(*bar_type)?);
        if bar_type.aggregation_source == AggregationSource::Internal
            && !self.aggregators.iter().any(|a| a.key == key)
        {
            let slot = self.instruments.intern(bar_type.instrument_id)?;
            // Validate the spec now, with the fixed-point precisions; the real aggregator takes
            // the precisions of the first update.
            BarAggregator::new(*bar_type, model::FIXED_PRECISION, model::FIXED_PRECISION)?;
            self.aggregators.push(AggregatorState {
                bar_type: *bar_type,
                slot,
                key,
                aggregator: None,
                armed_deadline: None,
                timer: None,
            })?;
        }
        self.subscribe_row(strategy, key.0, DataKind::Bar, cadence, None)
    }

    pub fn unsubscribe_bars(&mut self, strategy: StrategyIndex, bar_type: &BarType) -> Result<()> {
        let key = self.bars.slot_of(bar_type).ok_or(Status::NotFound)?;
        self.matrix.unsubscribe(key, DataKind::Bar, strategy)
    }

    /// A kernel feature of `kind` on `id`; identical declarations share one feature.
    pub fn declare_feature(
        &mut self,
        strategy: StrategyIndex,
        kind: FeatureKind,
        id: &InstrumentId,
        cadence: Cadence,
    ) -> Result<FeatureId> {
        if cadence == Cadence::OnBatch {
            return Err(Status::InvalidArgument);
        }
        let slot = self.instruments.intern(*id)?;
        let feature = self.features.declare(FeatureSpec { kind, slot })?;
        self.subscribe_row(strategy, feature.0, DataKind::Feature, cadence, None)?;
        Ok(feature)
    }

    fn subscribe_row(
        &mut self,
        strategy: StrategyIndex,
        row: u32,
        kind: DataKind,
        cadence: Cadence,
        slot: Option<InstrumentSlot>,
    ) -> Result<()> {
        if cadence == Cadence::OnBatch {
            let slot = slot.ok_or(Status::InvalidArgument)?;
            if !self
                .batches
                .iter()
                .any(|b| b.strategy == strategy && b.slot == slot && b.kind == kind)
            {
                let n = self.config.batch as usize;
                self.batches.push(BatchBuffer {
                    strategy,
                    slot,
                    kind,
                    trades: FixedVec::with_capacity(if kind == DataKind::Trade { n } else { 0 }),
                    quotes: FixedVec::with_capacity(if kind == DataKind::Quote { n } else { 0 }),
                })?;
            }
        }
        self.matrix.subscribe(row, kind, strategy, cadence)
    }

    // ---- strategies ---------------------------------------------------------------------------

    /// A strategy receives no callbacks once halted, or once every strategy was stopped.
    #[must_use]
    pub fn is_disabled(&self, s: StrategyIndex) -> bool {
        self.stopped || self.disabled.get(s.0 as usize).copied().unwrap_or(true)
    }

    /// The first failure of a strategy in a step wins.
    pub fn fail(&mut self, strategy: StrategyIndex, status: Status) {
        if self.failures.iter().any(|f| f.strategy == strategy) {
            return;
        }
        let _ = self.failures.push(StrategyFailure { strategy, status });
    }
}

impl State for Kernel {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.current.write(w);
        self.instruments.write(w);
        self.bars.write(w);
        self.matrix.write(w);
        self.features.write(w);
        write_books(&self.books, w);
        self.book_wanted.write(w);
        write_items(&self.aggregators, w);
        self.timers.write(w);
        self.timer_entries.write(w);
        self.pending.write(w);
        write_items(&self.batches, w);
        self.disabled.write(w);
        self.trading.write(w);
        self.node_state.write(w);
        self.started.write(w);
        self.stopped.write(w);
        self.stop_requested.write(w);
        self.halt_requested.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.current.read(r);
        self.instruments.read(r);
        self.bars.read(r);
        self.matrix.read(r);
        self.features.read(r);
        read_books(&mut self.books, &self.config, r);
        self.book_wanted.read(r);
        read_aggregators(&mut self.aggregators, r);
        self.timers.read(r);
        self.timer_entries.read(r);
        self.pending.read(r);
        read_batches(&mut self.batches, &self.config, r);
        self.disabled.read(r);
        self.trading.read(r);
        self.node_state.read(r);
        self.started.read(r);
        self.stopped.read(r);
        self.stop_requested.read(r);
        self.halt_requested.read(r);
        self.outputs.clear();
        self.failures.clear();
    }
}

/// A `FixedVec` of items without `Default`: capacity, length, items.
fn write_items<T: State>(items: &FixedVec<T>, w: &mut StateWriter<'_>) {
    w.u32(items.capacity() as u32);
    w.u32(items.len() as u32);
    for item in items {
        item.write(w);
    }
}

/// Books have no `Default`: each is written with its shape and rebuilt from it.
fn write_books(books: &FixedVec<Option<OrderBook>>, w: &mut StateWriter<'_>) {
    w.u32(books.capacity() as u32);
    w.u32(books.len() as u32);
    for book in books {
        match book {
            None => w.u8(0),
            Some(b) => {
                w.u8(1);
                b.config().price_increment.write(w);
                w.u8(b.config().size_precision);
                b.write(w);
            }
        }
    }
}

fn read_books(
    books: &mut FixedVec<Option<OrderBook>>,
    config: &KernelConfig,
    r: &mut StateReader<'_>,
) {
    let capacity = r.u32() as usize;
    let len = r.u32() as usize;
    if capacity != books.capacity() || len != books.len() {
        r.fail(Status::InvalidState);
        return;
    }
    for book in books.iter_mut() {
        let present = r.u8();
        if present > 1 {
            r.fail(Status::InvalidArgument);
            return;
        }
        if present == 0 {
            *book = None;
            continue;
        }
        let mut price_increment = Price::default();
        price_increment.read(r);
        let size_precision = r.u8();
        if !r.is_ok() {
            return;
        }
        let shape = BookConfig {
            price_increment,
            size_precision,
            window_levels: config.book_window_levels,
            overflow_levels: config.book_overflow_levels,
        };
        let mut shell = match OrderBook::new(shape) {
            Ok(b) => b,
            Err(e) => {
                r.fail(e);
                return;
            }
        };
        shell.read(r);
        *book = Some(shell);
    }
}

fn read_aggregators(aggregators: &mut FixedVec<AggregatorState>, r: &mut StateReader<'_>) {
    let capacity = r.u32() as usize;
    let len = r.u32() as usize;
    if capacity != aggregators.capacity() || len > capacity {
        r.fail(Status::InvalidState);
        return;
    }
    aggregators.clear();
    for _ in 0..len {
        let mut a = AggregatorState {
            bar_type: BarType::default(),
            slot: InstrumentSlot(0),
            key: BarKey(0),
            aggregator: None,
            armed_deadline: None,
            timer: None,
        };
        a.read(r);
        if !r.is_ok() {
            return;
        }
        let _ = aggregators.push(a);
    }
}

fn read_batches(
    batches: &mut FixedVec<BatchBuffer>,
    config: &KernelConfig,
    r: &mut StateReader<'_>,
) {
    let capacity = r.u32() as usize;
    let len = r.u32() as usize;
    if capacity != batches.capacity() || len > capacity {
        r.fail(Status::InvalidState);
        return;
    }
    batches.clear();
    let n = config.batch as usize;
    for _ in 0..len {
        let mut b = BatchBuffer {
            strategy: StrategyIndex(0),
            slot: InstrumentSlot(0),
            kind: DataKind::Trade,
            trades: FixedVec::with_capacity(n),
            quotes: FixedVec::with_capacity(n),
        };
        b.read(r);
        if !r.is_ok() {
            return;
        }
        let _ = batches.push(b);
    }
}
