//! Price-level order books (docs/architecture.md section 7.5). Prices are indexed by tick
//! (`price.raw / price_increment.raw`), so a level lookup is an array access. Levels inside a dense
//! window of `window_levels` ticks live in per-side arrays with an occupancy bitmap; levels outside
//! the window live in a small sorted overflow list, so the book stays exact when prices move far.
//! The window recentres on the mid when the touch leaves its middle half. All storage is sized at
//! construction.

use kernel_core::state::{read_sparse, write_sparse, State, StateReader, StateWriter};
use kernel_core::{FixedVec, Result, Status, UnixNanos};
use model::data::{OrderBookDelta, OrderBookDeltas, QuoteTick};
use model::enums::{BookAction, BookType, OrderSide};
use model::{Price, Quantity};

/// A side of the book.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Side {
    Bid,
    Ask,
}

impl From<OrderSide> for Side {
    fn from(side: OrderSide) -> Self {
        match side {
            OrderSide::Buy => Side::Bid,
            OrderSide::Sell => Side::Ask,
        }
    }
}

/// One price level.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct BookLevel {
    pub price: Price,
    pub size: Quantity,
}

/// The shape of a book, from the instrument and the node's capacities.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BookConfig {
    pub price_increment: Price,
    pub size_precision: u8,
    /// Dense ticks per side; rounded up to a multiple of 64.
    pub window_levels: u32,
    /// Levels per side that may sit outside the window.
    pub overflow_levels: u32,
}

const WORD: usize = 64;

/// One side: dense sizes over `[base, base + window)` with an occupancy bitmap, and a sorted
/// overflow list of `(tick, size)` for levels outside the window.
#[derive(Clone, Debug)]
struct SideLevels {
    sizes: FixedVec<u64>,
    words: FixedVec<u64>,
    overflow: FixedVec<(i64, u64)>,
}

impl SideLevels {
    fn new(window: usize, overflow: usize) -> Self {
        let mut sizes = FixedVec::with_capacity(window);
        let mut words = FixedVec::with_capacity(window / WORD);
        for _ in 0..window {
            let _ = sizes.push(0);
        }
        for _ in 0..window / WORD {
            let _ = words.push(0);
        }
        Self { sizes, words, overflow: FixedVec::with_capacity(overflow) }
    }

    fn clear(&mut self) {
        self.sizes.iter_mut().for_each(|s| *s = 0);
        self.words.iter_mut().for_each(|w| *w = 0);
        self.overflow.clear();
    }

    fn set_dense(&mut self, index: usize, size: u64) {
        self.sizes[index] = size;
        let bit = 1u64 << (index % WORD);
        if size == 0 {
            self.words[index / WORD] &= !bit;
        } else {
            self.words[index / WORD] |= bit;
        }
    }

    fn set_overflow(&mut self, tick: i64, size: u64) -> Result<()> {
        match self.overflow.binary_search_by_key(&tick, |&(t, _)| t) {
            Ok(i) => {
                if size == 0 {
                    self.overflow.remove_at(i);
                } else {
                    self.overflow[i].1 = size;
                }
                Ok(())
            }
            Err(i) => {
                if size == 0 {
                    return Ok(());
                }
                self.overflow.insert_at(i, (tick, size))
            }
        }
    }

    /// Occupied dense indices, from the touch outwards.
    fn dense_indices(&self, bids: bool) -> impl Iterator<Item = usize> + '_ {
        let n = self.sizes.len();
        let forward = (0..n).filter(move |&i| self.sizes[i] != 0);
        let backward = (0..n).rev().filter(move |&i| self.sizes[i] != 0);
        if bids {
            Either::A(backward)
        } else {
            Either::B(forward)
        }
    }

    /// The occupied dense index nearest the touch, using the bitmap words.
    fn best_dense(&self, bids: bool) -> Option<usize> {
        if bids {
            self.words
                .iter()
                .enumerate()
                .rev()
                .find(|(_, w)| **w != 0)
                .map(|(i, w)| i * WORD + (WORD - 1 - w.leading_zeros() as usize))
        } else {
            self.words
                .iter()
                .enumerate()
                .find(|(_, w)| **w != 0)
                .map(|(i, w)| i * WORD + w.trailing_zeros() as usize)
        }
    }

    fn rebuild_words(&mut self) {
        for w in &mut self.words {
            *w = 0;
        }
        for i in 0..self.sizes.len() {
            if self.sizes[i] != 0 {
                self.words[i / WORD] |= 1u64 << (i % WORD);
            }
        }
    }
}

/// Two iterators behind one type, so a side can hand out its levels in either direction.
enum Either<A, B> {
    A(A),
    B(B),
}
impl<T, A: Iterator<Item = T>, B: Iterator<Item = T>> Iterator for Either<A, B> {
    type Item = T;
    fn next(&mut self) -> Option<T> {
        match self {
            Either::A(a) => a.next(),
            Either::B(b) => b.next(),
        }
    }
}

/// One instrument's book.
#[derive(Clone, Debug)]
pub struct OrderBook {
    config: BookConfig,
    book_type: Option<BookType>,
    base: i64,
    bids: SideLevels,
    asks: SideLevels,
    ts_last: UnixNanos,
    sequence: u64,
}

impl OrderBook {
    /// `InvalidArgument` for a non-positive increment or a zero window.
    pub fn new(config: BookConfig) -> Result<Self> {
        if !config.price_increment.is_positive()
            || config.window_levels == 0
            || config.size_precision > model::FIXED_PRECISION
        {
            return Err(Status::InvalidArgument);
        }
        let window = (config.window_levels as usize).div_ceil(WORD) * WORD;
        let overflow = config.overflow_levels as usize;
        Ok(Self {
            config: BookConfig { window_levels: window as u32, ..config },
            book_type: None,
            base: 0,
            bids: SideLevels::new(window, overflow),
            asks: SideLevels::new(window, overflow),
            ts_last: UnixNanos::default(),
            sequence: 0,
        })
    }

    #[must_use]
    pub const fn config(&self) -> &BookConfig {
        &self.config
    }
    /// `None` before any update; then `L1_MBP` (fed by quotes) or `L2_MBP` (fed by deltas).
    #[must_use]
    pub const fn book_type(&self) -> Option<BookType> {
        self.book_type
    }
    #[must_use]
    pub const fn ts_last(&self) -> UnixNanos {
        self.ts_last
    }
    #[must_use]
    pub const fn sequence(&self) -> u64 {
        self.sequence
    }

    // ---- updates ------------------------------------------------------------------------------

    /// Applies one L2 delta: `Add` and `Update` set the level's size, `Delete` removes it, `Clear`
    /// empties the book (the start of a snapshot). `InvalidArgument` for a price off the grid or a
    /// book already fed by quotes.
    pub fn apply_delta(&mut self, d: &OrderBookDelta) -> Result<()> {
        if self.book_type == Some(BookType::L1Mbp) {
            return Err(Status::InvalidState);
        }
        self.book_type = Some(BookType::L2Mbp);
        match d.action {
            BookAction::Clear => {
                self.bids.clear();
                self.asks.clear();
            }
            BookAction::Add | BookAction::Update | BookAction::Delete => {
                let side: Side = d.order.side.ok_or(Status::InvalidArgument)?.into();
                let tick = self.tick_of(d.order.price)?;
                let size =
                    if d.action == BookAction::Delete { 0 } else { Self::size_raw(d.order.size)? };
                self.set_level(side, tick, size)?;
            }
        }
        self.ts_last = d.ts_init;
        self.sequence = d.sequence;
        self.maybe_recentre()
    }

    /// Applies a batch in order; stops at the first failure.
    pub fn apply_deltas(&mut self, deltas: &OrderBookDeltas) -> Result<()> {
        deltas.deltas.iter().try_for_each(|d| self.apply_delta(d))
    }

    /// L1: the quote replaces the top of each side. `InvalidState` for a book fed by deltas.
    pub fn apply_quote(&mut self, q: &QuoteTick) -> Result<()> {
        if self.book_type == Some(BookType::L2Mbp) {
            return Err(Status::InvalidState);
        }
        self.book_type = Some(BookType::L1Mbp);
        let bid = self.tick_of(q.bid_price)?;
        let ask = self.tick_of(q.ask_price)?;
        let bid_size = Self::size_raw(q.bid_size)?;
        let ask_size = Self::size_raw(q.ask_size)?;
        self.bids.clear();
        self.asks.clear();
        self.set_level(Side::Bid, bid, bid_size)?;
        self.set_level(Side::Ask, ask, ask_size)?;
        self.ts_last = q.ts_init;
        self.maybe_recentre()
    }

    pub fn clear(&mut self) {
        self.bids.clear();
        self.asks.clear();
    }

    // ---- queries ------------------------------------------------------------------------------

    #[must_use]
    pub fn best(&self, side: Side) -> Option<BookLevel> {
        let tick = self.best_tick(side)?;
        let size = self.size_at_tick(side, tick);
        Some(BookLevel { price: self.price_of(tick), size: self.quantity(size) })
    }
    #[must_use]
    pub fn best_bid(&self) -> Option<BookLevel> {
        self.best(Side::Bid)
    }
    #[must_use]
    pub fn best_ask(&self) -> Option<BookLevel> {
        self.best(Side::Ask)
    }

    /// The spread in ticks; `None` with a side empty.
    #[must_use]
    pub fn spread_ticks(&self) -> Option<i64> {
        Some(self.best_tick(Side::Ask)? - self.best_tick(Side::Bid)?)
    }

    /// Writes up to `out.len()` levels from the touch outwards; returns how many.
    pub fn levels(&self, side: Side, out: &mut [BookLevel]) -> usize {
        let mut n = 0;
        for (tick, size) in self.ticks(side) {
            if n == out.len() {
                break;
            }
            out[n] = BookLevel { price: self.price_of(tick), size: self.quantity(size) };
            n += 1;
        }
        n
    }

    /// Levels from the touch outwards as `(tick, size_raw)`.
    pub fn ticks(&self, side: Side) -> impl Iterator<Item = (i64, u64)> + '_ {
        let bids = side == Side::Bid;
        let levels = self.side(side);
        let base = self.base;
        let dense = levels.dense_indices(bids).map(move |i| (base + i as i64, levels.sizes[i]));
        let overflow = if bids {
            Either::A(levels.overflow.iter().rev().copied())
        } else {
            Either::B(levels.overflow.iter().copied())
        };
        Merge { a: dense.peekable(), b: overflow.peekable(), descending: bids }
    }

    /// Size at an exact price; zero when the level is empty or the price is off the grid.
    #[must_use]
    pub fn size_at(&self, side: Side, price: Price) -> Quantity {
        match self.tick_of(price) {
            Ok(tick) => self.quantity(self.size_at_tick(side, tick)),
            Err(_) => self.quantity(0),
        }
    }

    /// Total size of the first `depth` levels of a side.
    #[must_use]
    pub fn depth_size(&self, side: Side, depth: usize) -> Quantity {
        let raw = self.ticks(side).take(depth).map(|(_, s)| s).fold(0u64, u64::saturating_add);
        self.quantity(raw)
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.best_tick(Side::Bid).is_none() && self.best_tick(Side::Ask).is_none()
    }

    #[must_use]
    pub fn view(&self) -> BookView<'_> {
        BookView { book: self }
    }

    // ---- ticks and grid -----------------------------------------------------------------------

    /// The tick of a price on the grid; `InvalidArgument` off it.
    pub fn tick_of(&self, price: Price) -> Result<i64> {
        let inc = self.config.price_increment.raw();
        if price.is_undef() || price.raw() % inc != 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(price.raw() / inc)
    }

    #[must_use]
    pub fn price_of(&self, tick: i64) -> Price {
        let raw = tick.saturating_mul(self.config.price_increment.raw());
        Price::from_raw(raw, self.config.price_increment.precision())
            .unwrap_or_else(|_| Price::undef())
    }

    fn quantity(&self, raw: u64) -> Quantity {
        Quantity::from_raw(raw, self.config.size_precision).unwrap_or_else(|_| Quantity::undef())
    }

    fn size_raw(size: Quantity) -> Result<u64> {
        if size.is_undef() {
            return Err(Status::InvalidArgument);
        }
        Ok(size.raw())
    }

    // ---- storage ------------------------------------------------------------------------------

    fn side(&self, side: Side) -> &SideLevels {
        match side {
            Side::Bid => &self.bids,
            Side::Ask => &self.asks,
        }
    }
    fn side_mut(&mut self, side: Side) -> &mut SideLevels {
        match side {
            Side::Bid => &mut self.bids,
            Side::Ask => &mut self.asks,
        }
    }

    fn window(&self) -> i64 {
        i64::from(self.config.window_levels)
    }
    fn in_window(&self, tick: i64) -> bool {
        tick >= self.base && tick < self.base + self.window()
    }

    fn set_level(&mut self, side: Side, tick: i64, size: u64) -> Result<()> {
        if self.is_empty() && size != 0 && !self.in_window(tick) {
            // The first level decides where the window sits.
            self.base = tick - self.window() / 2;
        }
        if self.in_window(tick) {
            let index = (tick - self.base) as usize;
            self.side_mut(side).set_dense(index, size);
            Ok(())
        } else {
            self.side_mut(side).set_overflow(tick, size)
        }
    }

    fn size_at_tick(&self, side: Side, tick: i64) -> u64 {
        let levels = self.side(side);
        if self.in_window(tick) {
            levels.sizes[(tick - self.base) as usize]
        } else {
            levels
                .overflow
                .binary_search_by_key(&tick, |&(t, _)| t)
                .map_or(0, |i| levels.overflow[i].1)
        }
    }

    fn best_tick(&self, side: Side) -> Option<i64> {
        let bids = side == Side::Bid;
        let levels = self.side(side);
        let dense = levels.best_dense(bids).map(|i| self.base + i as i64);
        let overflow =
            if bids { levels.overflow.last() } else { levels.overflow.first() }.map(|&(t, _)| t);
        match (dense, overflow) {
            (Some(d), Some(o)) => Some(if bids { d.max(o) } else { d.min(o) }),
            (d, o) => d.or(o),
        }
    }

    /// Recentres the window on the mid when the touch leaves the middle half of the window.
    fn maybe_recentre(&mut self) -> Result<()> {
        let (Some(bid), Some(ask)) = (self.best_tick(Side::Bid), self.best_tick(Side::Ask)) else {
            return Ok(());
        };
        let mid = (bid + ask) / 2;
        let centre = self.base + self.window() / 2;
        if (mid - centre).abs() <= self.window() / 4 {
            return Ok(());
        }
        self.recentre(mid - self.window() / 2)
    }

    fn recentre(&mut self, new_base: i64) -> Result<()> {
        let shift = new_base - self.base;
        if shift == 0 {
            return Ok(());
        }
        let window = self.window();
        let old_base = self.base;
        for side in [Side::Bid, Side::Ask] {
            let levels = self.side_mut(side);
            let n = levels.sizes.len();
            // Move dense levels to their new index; walk in the direction that never overwrites a
            // level not yet moved. Levels leaving the window go to the overflow.
            let order: Either<_, _> =
                if shift > 0 { Either::A(0..n) } else { Either::B((0..n).rev()) };
            for i in order {
                let size = levels.sizes[i];
                if size == 0 {
                    continue;
                }
                levels.sizes[i] = 0;
                let new_index = i as i64 - shift;
                if (0..window).contains(&new_index) {
                    levels.sizes[new_index as usize] = size;
                } else {
                    levels.set_overflow(old_base + i as i64, size)?;
                }
            }
            // Overflow levels now inside the window come in.
            let mut i = 0;
            while i < levels.overflow.len() {
                let (tick, size) = levels.overflow[i];
                if tick >= new_base && tick < new_base + window {
                    levels.overflow.remove_at(i);
                    levels.sizes[(tick - new_base) as usize] = size;
                } else {
                    i += 1;
                }
            }
            levels.rebuild_words();
        }
        self.base = new_base;
        Ok(())
    }
}

/// Merges two touch-first level streams into one.
struct Merge<A: Iterator<Item = (i64, u64)>, B: Iterator<Item = (i64, u64)>> {
    a: core::iter::Peekable<A>,
    b: core::iter::Peekable<B>,
    descending: bool,
}
impl<A: Iterator<Item = (i64, u64)>, B: Iterator<Item = (i64, u64)>> Iterator for Merge<A, B> {
    type Item = (i64, u64);
    fn next(&mut self) -> Option<(i64, u64)> {
        match (self.a.peek(), self.b.peek()) {
            (Some(&(ta, _)), Some(&(tb, _))) => {
                let take_a = if self.descending { ta >= tb } else { ta <= tb };
                if take_a {
                    self.a.next()
                } else {
                    self.b.next()
                }
            }
            (Some(_), None) => self.a.next(),
            (None, Some(_)) => self.b.next(),
            (None, None) => None,
        }
    }
}

/// Read-only view of one instrument's book, valid during the callback that receives it.
#[derive(Clone, Copy, Debug)]
pub struct BookView<'a> {
    book: &'a OrderBook,
}

impl BookView<'_> {
    #[must_use]
    pub fn best_bid(&self) -> Option<BookLevel> {
        self.book.best_bid()
    }
    #[must_use]
    pub fn best_ask(&self) -> Option<BookLevel> {
        self.book.best_ask()
    }
    pub fn bids(&self, out: &mut [BookLevel]) -> usize {
        self.book.levels(Side::Bid, out)
    }
    pub fn asks(&self, out: &mut [BookLevel]) -> usize {
        self.book.levels(Side::Ask, out)
    }
    #[must_use]
    pub fn ts_last(&self) -> UnixNanos {
        self.book.ts_last()
    }
    #[must_use]
    pub fn book_type(&self) -> Option<BookType> {
        self.book.book_type()
    }
}

/// Snapshot encoding: the shape (checked against the restoring book's), the window's position,
/// the mostly empty dense arrays as runs, and the overflow lists.
impl State for OrderBook {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.config.window_levels);
        w.u32(self.config.overflow_levels);
        self.config.price_increment.write(w);
        w.u8(self.config.size_precision);
        self.book_type.map_or(0u8, |t| t as u8).write(w);
        (self.base as u64).write(w);
        self.ts_last.write(w);
        self.sequence.write(w);
        for side in [&self.bids, &self.asks] {
            write_sparse(w, &side.sizes);
            w.u32(side.overflow.len() as u32);
            for &(tick, size) in &side.overflow {
                w.u64(tick as u64);
                w.u64(size);
            }
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let window = r.u32();
        let overflow = r.u32();
        let mut increment = Price::default();
        increment.read(r);
        let precision = r.u8();
        if (window, overflow, increment, precision)
            != (
                self.config.window_levels,
                self.config.overflow_levels,
                self.config.price_increment,
                self.config.size_precision,
            )
        {
            r.fail(Status::CapacityExceeded);
            return;
        }
        self.book_type = match r.u8() {
            0 => None,
            t => match BookType::from_value(t) {
                Ok(t) => Some(t),
                Err(e) => {
                    r.fail(e);
                    return;
                }
            },
        };
        self.base = r.u64() as i64;
        self.ts_last.read(r);
        self.sequence = r.u64();
        for side in [&mut self.bids, &mut self.asks] {
            read_sparse(r, &mut side.sizes);
            let n = kernel_core::state::read_length(r, side.overflow.capacity());
            side.overflow.clear();
            for _ in 0..n {
                let tick = r.u64() as i64;
                let size = r.u64();
                if side.overflow.last().is_some_and(|&(t, _)| t >= tick) || size == 0 {
                    r.fail(Status::InvalidState);
                    return;
                }
                let _ = side.overflow.push((tick, size));
            }
            side.rebuild_words();
        }
    }
}
