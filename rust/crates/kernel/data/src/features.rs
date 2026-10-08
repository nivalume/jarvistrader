//! Kernel features (docs/architecture.md section 7.5): indicators computed inside `step` in
//! integer fixed point, so Python strategies can subscribe to a value instead of every tick.
//! Every value is on the 10^9 scale; divisions truncate toward zero.
//!
//! | kind | value |
//! | --- | --- |
//! | `Ema { period }` | exponential moving average of trade prices, `alpha = 2 / (period + 1)` |
//! | `Vwap { window }` | volume-weighted average price of the last `window` trades |
//! | `Imbalance` | `(bid_size - ask_size) / (bid_size + ask_size)` of the latest quote, in `[-1, 1]` |
//! | `Microprice` | `(bid * ask_size + ask * bid_size) / (bid_size + ask_size)` of the latest quote |
//! | `RealizedVol { window }` | `sqrt(sum of squared simple returns)` over the last `window` trade-to-trade returns; not annualized |
//!
//! Declaring a feature sizes its window once; updates never allocate.

use core::fmt;

use kernel_core::int_math::isqrt;
use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{FixedVec, Result, Status};
use model::data::{QuoteTick, TradeTick};
use model::decimal;
use model::FIXED_SCALAR;

use crate::intern::InstrumentSlot;

const SCALE: i128 = FIXED_SCALAR as i128;

/// A feature value on the 10^9 scale.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FeatureValue(pub i64);

impl FeatureValue {
    fn from_i128(v: i128) -> Result<Self> {
        i64::try_from(v).map(FeatureValue).map_err(|_| Status::Overflow)
    }
}
impl fmt::Display for FeatureValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        decimal::fmt_raw(f, i128::from(self.0), model::FIXED_PRECISION)
    }
}
impl State for FeatureValue {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u64(self.0 as u64);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.0 = r.u64() as i64;
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum FeatureKind {
    Ema {
        period: u32,
    },
    Vwap {
        window: u32,
    },
    /// The placeholder a snapshot reads into before the real kind arrives.
    #[default]
    Imbalance,
    Microprice,
    RealizedVol {
        window: u32,
    },
}

impl FeatureKind {
    /// Whether the feature updates on trades (else on quotes).
    #[must_use]
    pub const fn uses_trades(self) -> bool {
        matches!(
            self,
            FeatureKind::Ema { .. } | FeatureKind::Vwap { .. } | FeatureKind::RealizedVol { .. }
        )
    }
    fn validate(self) -> Result<()> {
        match self {
            FeatureKind::Ema { period: 0 }
            | FeatureKind::Vwap { window: 0 }
            | FeatureKind::RealizedVol { window: 0 } => Err(Status::InvalidArgument),
            FeatureKind::Vwap { window } | FeatureKind::RealizedVol { window }
                if window > 1 << 20 =>
            {
                Err(Status::CapacityExceeded)
            }
            _ => Ok(()),
        }
    }
}

impl State for FeatureKind {
    fn write(&self, w: &mut StateWriter<'_>) {
        match self {
            FeatureKind::Ema { period } => {
                w.u8(1);
                w.u32(*period);
            }
            FeatureKind::Vwap { window } => {
                w.u8(2);
                w.u32(*window);
            }
            FeatureKind::Imbalance => w.u8(3),
            FeatureKind::Microprice => w.u8(4),
            FeatureKind::RealizedVol { window } => {
                w.u8(5);
                w.u32(*window);
            }
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        *self = match r.u8() {
            1 => FeatureKind::Ema { period: r.u32() },
            2 => FeatureKind::Vwap { window: r.u32() },
            3 => FeatureKind::Imbalance,
            4 => FeatureKind::Microprice,
            5 => FeatureKind::RealizedVol { window: r.u32() },
            _ => {
                r.fail(Status::InvalidArgument);
                return;
            }
        };
    }
}

/// A feature of one instrument.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct FeatureSpec {
    pub kind: FeatureKind,
    pub slot: InstrumentSlot,
}
kernel_core::state_fields!(FeatureSpec { kind, slot });

/// A fixed-capacity ring of samples, oldest evicted first.
#[derive(Clone, Debug)]
struct Ring<T> {
    items: FixedVec<T>,
    next: usize,
}

impl<T: Copy> Ring<T> {
    fn new(capacity: usize) -> Self {
        Self { items: FixedVec::with_capacity(capacity), next: 0 }
    }
    /// Adds `sample`; returns the sample it evicted, if the ring was full.
    fn push(&mut self, sample: T) -> Option<T> {
        if self.items.is_full() {
            let evicted = core::mem::replace(&mut self.items[self.next], sample);
            self.next = (self.next + 1) % self.items.capacity();
            Some(evicted)
        } else {
            let _ = self.items.push(sample);
            None
        }
    }
    fn len(&self) -> usize {
        self.items.len()
    }
}

impl<T: State + Default + Copy> State for Ring<T> {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.items.write(w);
        w.u32(self.next as u32);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.items.read(r);
        self.next = r.u32() as usize;
        if self.next >= self.items.capacity().max(1) {
            r.fail(Status::InvalidState);
        }
    }
}

/// A trade sample: `(price_raw, size_raw)`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Sample {
    price: i64,
    size: u64,
}
kernel_core::state_fields!(Sample { price, size });

#[derive(Clone, Debug)]
enum Engine {
    Ema { value: Option<i64>, period: u32 },
    Vwap { ring: Ring<Sample>, sum_pv: i128, sum_v: u128 },
    Imbalance,
    Microprice,
    RealizedVol { last_price: Option<i64>, ring: Ring<i64>, sum_sq: u128 },
}

/// One declared feature and its state.
#[derive(Clone, Debug)]
pub struct Feature {
    spec: FeatureSpec,
    engine: Engine,
    last: Option<FeatureValue>,
}

impl Feature {
    pub fn new(spec: FeatureSpec) -> Result<Self> {
        spec.kind.validate()?;
        let engine = match spec.kind {
            FeatureKind::Ema { period } => Engine::Ema { value: None, period },
            FeatureKind::Vwap { window } => {
                Engine::Vwap { ring: Ring::new(window as usize), sum_pv: 0, sum_v: 0 }
            }
            FeatureKind::Imbalance => Engine::Imbalance,
            FeatureKind::Microprice => Engine::Microprice,
            FeatureKind::RealizedVol { window } => Engine::RealizedVol {
                last_price: None,
                ring: Ring::new(window as usize),
                sum_sq: 0,
            },
        };
        Ok(Self { spec, engine, last: None })
    }

    #[must_use]
    pub const fn spec(&self) -> &FeatureSpec {
        &self.spec
    }
    /// The last value produced, if any.
    #[must_use]
    pub const fn last(&self) -> Option<FeatureValue> {
        self.last
    }

    /// Updates from a trade; `Some` when a new value exists. `InvalidState` for a quote feature.
    pub fn on_trade(&mut self, t: &TradeTick) -> Result<Option<FeatureValue>> {
        let price = t.price.raw();
        let value = match &mut self.engine {
            Engine::Ema { value, period } => {
                let next = match *value {
                    None => price,
                    Some(v) => {
                        let delta =
                            (i128::from(price) - i128::from(v)) * 2 / (i128::from(*period) + 1);
                        i64::try_from(i128::from(v) + delta).map_err(|_| Status::Overflow)?
                    }
                };
                *value = Some(next);
                Some(FeatureValue(next))
            }
            Engine::Vwap { ring, sum_pv, sum_v } => {
                let sample = Sample { price, size: t.size.raw() };
                if let Some(old) = ring.push(sample) {
                    *sum_pv -= i128::from(old.price) * i128::from(old.size);
                    *sum_v -= u128::from(old.size);
                }
                *sum_pv += i128::from(sample.price) * i128::from(sample.size);
                *sum_v += u128::from(sample.size);
                if *sum_v == 0 {
                    None
                } else {
                    Some(FeatureValue::from_i128(
                        *sum_pv / i128::try_from(*sum_v).map_err(|_| Status::Overflow)?,
                    )?)
                }
            }
            Engine::RealizedVol { last_price, ring, sum_sq } => {
                let previous = last_price.replace(price);
                let Some(previous) = previous else { return Ok(None) };
                if previous == 0 {
                    return Err(Status::InvalidArgument);
                }
                let ret = (i128::from(price) - i128::from(previous)) * SCALE / i128::from(previous);
                let ret = i64::try_from(ret).map_err(|_| Status::Overflow)?;
                if let Some(old) = ring.push(ret) {
                    *sum_sq -= (i128::from(old) * i128::from(old)) as u128;
                }
                *sum_sq += (i128::from(ret) * i128::from(ret)) as u128;
                if ring.len() == 0 {
                    None
                } else {
                    Some(FeatureValue::from_i128(i128::from(isqrt(*sum_sq)))?)
                }
            }
            Engine::Imbalance | Engine::Microprice => return Err(Status::InvalidState),
        };
        self.last = value;
        Ok(value)
    }

    /// Updates from a quote. `InvalidState` for a trade feature.
    pub fn on_quote(&mut self, q: &QuoteTick) -> Result<Option<FeatureValue>> {
        let bid_size = i128::from(q.bid_size.raw());
        let ask_size = i128::from(q.ask_size.raw());
        let total = bid_size + ask_size;
        let value = match &self.engine {
            Engine::Imbalance => {
                if total == 0 {
                    None
                } else {
                    Some(FeatureValue::from_i128((bid_size - ask_size) * SCALE / total)?)
                }
            }
            Engine::Microprice => {
                if total == 0 {
                    None
                } else {
                    let weighted = i128::from(q.bid_price.raw()) * ask_size
                        + i128::from(q.ask_price.raw()) * bid_size;
                    Some(FeatureValue::from_i128(weighted / total)?)
                }
            }
            _ => return Err(Status::InvalidState),
        };
        self.last = value;
        Ok(value)
    }
}

impl State for Feature {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.last.write(w);
        match &self.engine {
            Engine::Ema { value, .. } => value.write(w),
            Engine::Vwap { ring, sum_pv, sum_v } => {
                ring.write(w);
                sum_pv.write(w);
                sum_v.write(w);
            }
            Engine::Imbalance | Engine::Microprice => {}
            Engine::RealizedVol { last_price, ring, sum_sq } => {
                last_price.write(w);
                ring.write(w);
                sum_sq.write(w);
            }
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.last.read(r);
        match &mut self.engine {
            Engine::Ema { value, .. } => value.read(r),
            Engine::Vwap { ring, sum_pv, sum_v } => {
                ring.read(r);
                sum_pv.read(r);
                sum_v.read(r);
            }
            Engine::Imbalance | Engine::Microprice => {}
            Engine::RealizedVol { last_price, ring, sum_sq } => {
                last_price.read(r);
                ring.read(r);
                sum_sq.read(r);
            }
        }
    }
}

/// The id of a declared feature: its row in the subscription matrix.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FeatureId(pub u32);

impl FeatureId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// The declared features. Identical specs share one feature (and one id).
#[derive(Clone, Debug)]
pub struct FeatureGraph {
    features: FixedVec<Feature>,
}

impl FeatureGraph {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self { features: FixedVec::with_capacity(capacity) }
    }

    /// The id of the feature for `spec`, declaring it when new.
    pub fn declare(&mut self, spec: FeatureSpec) -> Result<FeatureId> {
        if let Some(i) = self.features.iter().position(|f| f.spec == spec) {
            return Ok(FeatureId(i as u32));
        }
        self.features.push(Feature::new(spec)?)?;
        Ok(FeatureId(self.features.len() as u32 - 1))
    }

    /// Feeds a trade of instrument `slot` to its trade features; `emit` receives each new value.
    pub fn on_trade(
        &mut self,
        slot: InstrumentSlot,
        t: &TradeTick,
        mut emit: impl FnMut(FeatureId, FeatureValue),
    ) -> Result<()> {
        for (i, f) in self.features.iter_mut().enumerate() {
            if f.spec.slot == slot && f.spec.kind.uses_trades() {
                if let Some(v) = f.on_trade(t)? {
                    emit(FeatureId(i as u32), v);
                }
            }
        }
        Ok(())
    }

    /// Feeds a quote of instrument `slot` to its quote features.
    pub fn on_quote(
        &mut self,
        slot: InstrumentSlot,
        q: &QuoteTick,
        mut emit: impl FnMut(FeatureId, FeatureValue),
    ) -> Result<()> {
        for (i, f) in self.features.iter_mut().enumerate() {
            if f.spec.slot == slot && !f.spec.kind.uses_trades() {
                if let Some(v) = f.on_quote(q)? {
                    emit(FeatureId(i as u32), v);
                }
            }
        }
        Ok(())
    }

    #[must_use]
    pub fn get(&self, id: FeatureId) -> Option<&Feature> {
        self.features.get(id.index())
    }
    #[must_use]
    pub fn len(&self) -> usize {
        self.features.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.features.is_empty()
    }
    pub fn iter(&self) -> impl Iterator<Item = (FeatureId, &Feature)> {
        self.features.iter().enumerate().map(|(i, f)| (FeatureId(i as u32), f))
    }
}

/// Declarations happen inside `step` and replay with the inputs, so the snapshot holds the specs
/// and each feature's state.
impl State for FeatureGraph {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.features.len() as u32);
        for f in &self.features {
            f.spec.write(w);
            f.write(w);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let n = kernel_core::state::read_length(r, self.features.capacity());
        self.features.clear();
        for _ in 0..n {
            let mut spec = FeatureSpec::default();
            spec.read(r);
            let mut feature = match Feature::new(spec) {
                Ok(f) => f,
                Err(e) => {
                    r.fail(e);
                    return;
                }
            };
            feature.read(r);
            let _ = self.features.push(feature);
        }
    }
}
