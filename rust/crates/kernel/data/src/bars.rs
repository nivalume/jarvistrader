//! Bar aggregation inside the kernel: INTERNAL bars built from trades (price type `LAST`) or
//! quotes (`BID`, `ASK`, `MID`). Supported aggregations: `TICK` (every `step` updates), `VOLUME`
//! (every `step` units; a trade that crosses the threshold is split across bars, as nautilus
//! does) and time (`MILLISECOND` to `WEEK`). Time bars cover `[start, end)` aligned to the Unix
//! epoch and close at `end` with `ts_event = ts_init = end`; the engine arms a timer for
//! [`BarAggregator::next_close`], and an update at or after `end` also closes the bar first.
//! Intervals without updates produce no bar.

use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{Result, Status, UnixNanos};
use model::bar::{Bar, BarType};
use model::data::{QuoteTick, TradeTick};
use model::enums::{AggregationSource, BarAggregation, PriceType};
use model::{Price, Quantity};

/// How the aggregator closes bars.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Tick { step: u32 },
    Volume { step_raw: u64 },
    Time { duration_ns: u64 },
}

/// The bar being built.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Partial {
    open: Price,
    high: Price,
    low: Price,
    close: Price,
    volume_raw: u64,
    count: u32,
    /// Time bars: the interval; others: the first update's time, for information.
    start: UnixNanos,
    end: UnixNanos,
}

kernel_core::state_fields!(Partial { open, high, low, close, volume_raw, count, start, end });

/// Up to two bars one update can complete: a time bar the update's timestamp closed, then a tick
/// or volume bar the update completed. A volume bar can complete several at once; see
/// [`Completed::overflow`].
#[derive(Clone, Copy, Debug, Default)]
pub struct Completed {
    bars: [Option<Bar>; 4],
    len: usize,
    /// Bars that did not fit (a trade larger than four volume steps); they are lost, so the engine
    /// counts them.
    pub overflow: u32,
}

impl Completed {
    fn push(&mut self, bar: Bar) {
        if self.len < self.bars.len() {
            self.bars[self.len] = Some(bar);
            self.len += 1;
        } else {
            self.overflow += 1;
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = &Bar> {
        self.bars[..self.len].iter().flatten()
    }
    #[must_use]
    pub const fn len(&self) -> usize {
        self.len
    }
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }
}

/// Builds the bars of one INTERNAL bar type.
#[derive(Clone, Debug)]
pub struct BarAggregator {
    bar_type: BarType,
    mode: Mode,
    price_precision: u8,
    size_precision: u8,
    open: Option<Partial>,
}

impl BarAggregator {
    /// `InvalidArgument` for an EXTERNAL or composite type, an unsupported aggregation, or a
    /// volume bar on quotes (quotes carry no traded volume).
    pub fn new(bar_type: BarType, price_precision: u8, size_precision: u8) -> Result<Self> {
        if bar_type.aggregation_source != AggregationSource::Internal || bar_type.is_composite() {
            return Err(Status::InvalidArgument);
        }
        let spec = bar_type.spec;
        let step = u64::from(spec.step);
        let mode = match spec.aggregation {
            BarAggregation::Tick => Mode::Tick { step: spec.step },
            BarAggregation::Volume => {
                if spec.price_type != PriceType::Last {
                    return Err(Status::InvalidArgument);
                }
                // `step` is in whole units of the instrument.
                let step_raw =
                    step.checked_mul(model::FIXED_SCALAR as u64).ok_or(Status::Overflow)?;
                Mode::Volume { step_raw }
            }
            _ => Mode::Time { duration_ns: spec.duration_ns().ok_or(Status::InvalidArgument)? },
        };
        if spec.price_type == PriceType::Mark
            || price_precision > model::FIXED_PRECISION
            || size_precision > model::FIXED_PRECISION
        {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { bar_type, mode, price_precision, size_precision, open: None })
    }

    #[must_use]
    pub const fn bar_type(&self) -> &BarType {
        &self.bar_type
    }
    /// Whether the aggregator consumes trades (`LAST`) or quotes (`BID`, `ASK`, `MID`).
    #[must_use]
    pub fn uses_trades(&self) -> bool {
        self.bar_type.spec.price_type == PriceType::Last
    }
    #[must_use]
    pub fn is_time_bar(&self) -> bool {
        matches!(self.mode, Mode::Time { .. })
    }
    /// Close time of the open time bar, if any: when the engine's timer should fire.
    #[must_use]
    pub fn next_close(&self) -> Option<UnixNanos> {
        match self.mode {
            Mode::Time { .. } => self.open.map(|o| o.end),
            _ => None,
        }
    }

    /// Feeds a trade (`LAST` bars). `InvalidState` for a quote-fed aggregator.
    pub fn on_trade(&mut self, t: &TradeTick) -> Result<Completed> {
        if !self.uses_trades() {
            return Err(Status::InvalidState);
        }
        self.update(t.price, t.size.raw(), t.ts_init)
    }

    /// Feeds a quote (`BID`, `ASK` or `MID` bars); size counts one update of zero volume.
    pub fn on_quote(&mut self, q: &QuoteTick) -> Result<Completed> {
        let price = match self.bar_type.spec.price_type {
            PriceType::Bid => q.bid_price,
            PriceType::Ask => q.ask_price,
            PriceType::Mid => q.mid_price()?,
            PriceType::Last | PriceType::Mark => return Err(Status::InvalidState),
        };
        self.update(price, 0, q.ts_init)
    }

    /// Closes the open time bar when `now` has reached its end (the engine's timer).
    pub fn on_time(&mut self, now: UnixNanos) -> Option<Bar> {
        let Mode::Time { .. } = self.mode else { return None };
        let open = self.open?;
        if now < open.end {
            return None;
        }
        self.open = None;
        self.build(&open, open.end)
    }

    fn update(&mut self, price: Price, size_raw: u64, ts: UnixNanos) -> Result<Completed> {
        if price.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let price = self.at_precision(price)?;
        let mut completed = Completed::default();
        match self.mode {
            Mode::Time { duration_ns } => {
                if let Some(open) = self.open {
                    if ts >= open.end {
                        self.open = None;
                        if let Some(bar) = self.build(&open, open.end) {
                            completed.push(bar);
                        }
                    }
                }
                let open = self.open.get_or_insert_with(|| {
                    let start = UnixNanos::new(ts.value() - ts.value() % duration_ns);
                    Partial {
                        open: price,
                        high: price,
                        low: price,
                        close: price,
                        start,
                        end: UnixNanos::new(start.value().saturating_add(duration_ns)),
                        ..Partial::default()
                    }
                });
                Self::add(open, price, size_raw);
            }
            Mode::Tick { step } => {
                let open = self.open.get_or_insert_with(|| Self::fresh(price, ts));
                Self::add(open, price, size_raw);
                if open.count >= step {
                    let done = *open;
                    self.open = None;
                    if let Some(bar) = self.build(&done, ts) {
                        completed.push(bar);
                    }
                }
            }
            Mode::Volume { step_raw } => {
                let mut remaining = size_raw;
                loop {
                    let open = self.open.get_or_insert_with(|| Self::fresh(price, ts));
                    let room = step_raw - open.volume_raw;
                    let take = remaining.min(room);
                    Self::add(open, price, take);
                    remaining -= take;
                    if open.volume_raw < step_raw {
                        break;
                    }
                    let done = *open;
                    self.open = None;
                    if let Some(bar) = self.build(&done, ts) {
                        completed.push(bar);
                    }
                    if remaining == 0 {
                        break;
                    }
                }
            }
        }
        Ok(completed)
    }

    fn fresh(price: Price, ts: UnixNanos) -> Partial {
        Partial {
            open: price,
            high: price,
            low: price,
            close: price,
            start: ts,
            end: ts,
            ..Partial::default()
        }
    }

    fn add(open: &mut Partial, price: Price, size_raw: u64) {
        open.high = open.high.max(price);
        open.low = open.low.min(price);
        open.close = price;
        open.volume_raw = open.volume_raw.saturating_add(size_raw);
        open.count = open.count.saturating_add(1);
    }

    fn build(&self, open: &Partial, ts: UnixNanos) -> Option<Bar> {
        let volume = Quantity::from_raw(open.volume_raw, self.size_precision).ok()?;
        Bar::new(self.bar_type, open.open, open.high, open.low, open.close, volume, ts, ts).ok()
    }

    /// Prices at the bar's precision: a quote mid may carry one more digit; truncate toward zero
    /// onto the instrument's grid.
    fn at_precision(&self, p: Price) -> Result<Price> {
        if p.precision() <= self.price_precision {
            return Price::from_raw(p.raw(), self.price_precision);
        }
        let step = kernel_core::int_math::POW10
            [(model::FIXED_PRECISION - self.price_precision) as usize] as i64;
        Price::from_raw(p.raw() - p.raw() % step, self.price_precision)
    }
}

/// The bar type and mode come from the declaration; the snapshot holds the open bar.
impl State for BarAggregator {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.open.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.open.read(r);
    }
}
