//! Typed subscriptions and delivery cadences (docs/architecture.md sections 7.2 and 7.5).
//!
//! The matrix has one row per slot (instrument slot for instrument data, bar key for bars,
//! feature id for features) and one column per [`DataKind`]. A cell lists its subscribers in
//! subscription order; that order is state, so a replay delivers identically. Storage is sized at
//! construction and subscribing never allocates.

use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{FixedVec, Result, Status, UnixNanos};

/// What a subscription delivers. Each kind has its own column in the matrix.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum DataKind {
    Trade = 0,
    Quote = 1,
    Book = 2,
    Bar = 3,
    MarkPrice = 4,
    IndexPrice = 5,
    FundingRate = 6,
    InstrumentStatus = 7,
    InstrumentClose = 8,
    Liquidation = 9,
    Instrument = 10,
    Feature = 11,
}

impl DataKind {
    pub const COUNT: usize = 12;
    pub const ALL: [DataKind; Self::COUNT] = [
        DataKind::Trade,
        DataKind::Quote,
        DataKind::Book,
        DataKind::Bar,
        DataKind::MarkPrice,
        DataKind::IndexPrice,
        DataKind::FundingRate,
        DataKind::InstrumentStatus,
        DataKind::InstrumentClose,
        DataKind::Liquidation,
        DataKind::Instrument,
        DataKind::Feature,
    ];

    /// Which table the row of this kind indexes.
    #[must_use]
    pub const fn row_space(self) -> RowSpace {
        match self {
            DataKind::Bar => RowSpace::BarKey,
            DataKind::Feature => RowSpace::FeatureId,
            _ => RowSpace::InstrumentSlot,
        }
    }
}

/// The three row spaces of the matrix.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RowSpace {
    InstrumentSlot,
    BarKey,
    FeatureId,
}

kernel_core::state_enum!(DataKind: u8 {
    Trade, Quote, Book, Bar, MarkPrice, IndexPrice, FundingRate, InstrumentStatus, InstrumentClose,
    Liquidation, Instrument, Feature
});

/// When a subscriber sees updates.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Cadence {
    /// Each update, as it is processed.
    #[default]
    Every,
    /// Once per batch, the latest update of the batch, delivered at `BatchEnd`.
    Conflated,
    /// The first update in each period of `period_ns`, periods aligned to the Unix epoch: at most
    /// one delivery per period, never delayed.
    Sampled { period_ns: u64 },
    /// Once per batch, all updates of the batch together (`on_batch`).
    OnBatch,
}

impl Cadence {
    pub fn sampled_ms(ms: u64) -> Result<Self> {
        let period_ns = ms.checked_mul(1_000_000).ok_or(Status::Overflow)?;
        Self::sampled_ns(period_ns)
    }
    pub fn sampled_ns(period_ns: u64) -> Result<Self> {
        if period_ns == 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(Cadence::Sampled { period_ns })
    }
}

impl State for Cadence {
    fn write(&self, w: &mut StateWriter<'_>) {
        match self {
            Cadence::Every => w.u8(0),
            Cadence::Conflated => w.u8(1),
            Cadence::Sampled { period_ns } => {
                w.u8(2);
                w.u64(*period_ns);
            }
            Cadence::OnBatch => w.u8(3),
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        *self = match r.u8() {
            0 => Cadence::Every,
            1 => Cadence::Conflated,
            2 => match Cadence::sampled_ns(r.u64()) {
                Ok(c) => c,
                Err(e) => {
                    r.fail(e);
                    return;
                }
            },
            3 => Cadence::OnBatch,
            _ => {
                r.fail(Status::InvalidArgument);
                return;
            }
        };
    }
}

/// The position of a strategy in the node's strategy set.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct StrategyIndex(pub u16);

impl State for StrategyIndex {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u16(self.0);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.0 = r.u16();
    }
}

/// What the engine does with an update for one subscriber.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    /// Deliver this update now.
    Now,
    /// Keep the latest update (`Conflated`) or buffer it (`OnBatch`); deliver at `BatchEnd`.
    Defer,
    /// Nothing: a `Sampled` subscriber already saw this period.
    Skip,
}

/// One subscriber of a matrix cell and its delivery state.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Subscriber {
    pub strategy: StrategyIndex,
    pub cadence: Cadence,
    /// `Sampled`: the period of the last delivery, as `ts / period`.
    last_period: Option<u64>,
    /// `Conflated` / `OnBatch`: an update is waiting for `BatchEnd`.
    pending: bool,
}

impl Subscriber {
    #[must_use]
    pub const fn new(strategy: StrategyIndex, cadence: Cadence) -> Self {
        Self { strategy, cadence, last_period: None, pending: false }
    }

    /// Decides for an update at `ts_init` and records what it decided.
    pub fn on_update(&mut self, ts_init: UnixNanos) -> Decision {
        match self.cadence {
            Cadence::Every => Decision::Now,
            Cadence::Sampled { period_ns } => {
                let period = ts_init.value() / period_ns;
                if self.last_period == Some(period) {
                    Decision::Skip
                } else {
                    self.last_period = Some(period);
                    Decision::Now
                }
            }
            Cadence::Conflated | Cadence::OnBatch => {
                self.pending = true;
                Decision::Defer
            }
        }
    }

    /// At `BatchEnd`: whether a deferred delivery is due; clears it.
    pub fn on_batch_end(&mut self) -> bool {
        core::mem::take(&mut self.pending)
    }

    #[must_use]
    pub const fn is_pending(&self) -> bool {
        self.pending
    }
}

kernel_core::state_fields!(Subscriber { strategy, cadence, last_period, pending });

/// Subscribers per (row, kind). `rows` is the largest row space (the instrument capacity, the
/// bar capacity or the feature capacity, whichever is largest); each cell holds up to
/// `strategies` subscribers.
#[derive(Clone, Debug)]
pub struct SubscriptionMatrix {
    rows: u32,
    cells: FixedVec<FixedVec<Subscriber>>,
}

impl SubscriptionMatrix {
    #[must_use]
    pub fn new(rows: u32, strategies: u16) -> Self {
        let n = rows as usize * DataKind::COUNT;
        let mut cells = FixedVec::with_capacity(n);
        for _ in 0..n {
            let _ = cells.push(FixedVec::with_capacity(strategies as usize));
        }
        Self { rows, cells }
    }

    /// Adds the subscription of `strategy` to (row, kind), or changes its cadence (the delivery
    /// state restarts).
    pub fn subscribe(
        &mut self,
        row: u32,
        kind: DataKind,
        strategy: StrategyIndex,
        cadence: Cadence,
    ) -> Result<()> {
        let cell = self.cell_mut(row, kind)?;
        if let Some(existing) = cell.iter_mut().find(|s| s.strategy == strategy) {
            *existing = Subscriber::new(strategy, cadence);
            return Ok(());
        }
        cell.push(Subscriber::new(strategy, cadence))
    }

    /// Removes the subscription, keeping the order of the others; `NotFound` when absent.
    pub fn unsubscribe(&mut self, row: u32, kind: DataKind, strategy: StrategyIndex) -> Result<()> {
        let cell = self.cell_mut(row, kind)?;
        let i = cell.iter().position(|s| s.strategy == strategy).ok_or(Status::NotFound)?;
        cell.retain(|s| s.strategy != strategy);
        debug_assert!(i < cell.len() + 1);
        Ok(())
    }

    /// The cell's subscribers, in subscription order; empty for a row out of range.
    #[must_use]
    pub fn subscribers(&self, row: u32, kind: DataKind) -> &[Subscriber] {
        self.cells.get(Self::index(row, kind)).map_or(&[], |c| c.as_slice())
    }
    pub fn subscribers_mut(&mut self, row: u32, kind: DataKind) -> &mut [Subscriber] {
        match self.cells.get_mut(Self::index(row, kind)) {
            Some(c) => c.as_mut_slice(),
            None => &mut [],
        }
    }

    #[must_use]
    pub fn find(&self, row: u32, kind: DataKind, strategy: StrategyIndex) -> Option<&Subscriber> {
        self.subscribers(row, kind).iter().find(|s| s.strategy == strategy)
    }

    /// Every subscriber of every cell, for `BatchEnd` sweeps: `(row, kind, subscriber)`.
    pub fn iter_mut(&mut self) -> impl Iterator<Item = (u32, DataKind, &mut Subscriber)> {
        self.cells.iter_mut().enumerate().flat_map(|(i, cell)| {
            let row = (i / DataKind::COUNT) as u32;
            let kind = DataKind::ALL[i % DataKind::COUNT];
            cell.iter_mut().map(move |s| (row, kind, s))
        })
    }

    #[must_use]
    pub const fn rows(&self) -> u32 {
        self.rows
    }

    fn index(row: u32, kind: DataKind) -> usize {
        row as usize * DataKind::COUNT + kind as usize
    }
    fn cell_mut(&mut self, row: u32, kind: DataKind) -> Result<&mut FixedVec<Subscriber>> {
        if row >= self.rows {
            return Err(Status::OutOfRange);
        }
        self.cells.get_mut(Self::index(row, kind)).ok_or(Status::OutOfRange)
    }
}

/// The shape comes from the configuration; the snapshot holds each cell's subscribers.
impl State for SubscriptionMatrix {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.rows);
        for cell in &self.cells {
            cell.write(w);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        if r.u32() != self.rows {
            r.fail(Status::CapacityExceeded);
            return;
        }
        for cell in &mut self.cells {
            cell.read(r);
        }
    }
}
