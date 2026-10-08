//! Bars and their types (docs/architecture.md section 6.4).

use core::fmt;
use core::str::FromStr;

use kernel_core::{Result, Status, UnixNanos};

use crate::enums::{AggregationSource, BarAggregation, PriceType};
use crate::fixed_point::{Price, Quantity};
use crate::identifiers::InstrumentId;
use crate::wire_struct;

/// `{step}-{AGG}-{PRICE_TYPE}`, for example `1-MINUTE-LAST`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BarSpecification {
    pub step: u32,
    pub aggregation: BarAggregation,
    pub price_type: PriceType,
}

impl BarSpecification {
    /// `step` must be positive.
    pub fn new(step: u32, aggregation: BarAggregation, price_type: PriceType) -> Result<Self> {
        if step == 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { step, aggregation, price_type })
    }

    pub fn parse(text: &str) -> Result<Self> {
        let mut parts = text.splitn(3, '-');
        let step = parts.next().ok_or(Status::ParseError)?;
        let agg = parts.next().ok_or(Status::ParseError)?;
        let price = parts.next().ok_or(Status::ParseError)?;
        if step.is_empty() || !step.bytes().all(|b| b.is_ascii_digit()) {
            return Err(Status::ParseError);
        }
        let step: u32 = step.parse().map_err(|_| Status::OutOfRange)?;
        Self::new(step, BarAggregation::from_text(agg)?, PriceType::from_text(price)?)
    }

    /// Time-based aggregations have a fixed duration; the others do not.
    #[must_use]
    pub fn duration_ns(&self) -> Option<u64> {
        let unit: u64 = match self.aggregation {
            BarAggregation::Millisecond => 1_000_000,
            BarAggregation::Second => 1_000_000_000,
            BarAggregation::Minute => 60 * 1_000_000_000,
            BarAggregation::Hour => 3_600 * 1_000_000_000,
            BarAggregation::Day => 86_400 * 1_000_000_000,
            BarAggregation::Week => 7 * 86_400 * 1_000_000_000,
            _ => return None,
        };
        unit.checked_mul(u64::from(self.step))
    }
}
impl fmt::Display for BarSpecification {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}-{}-{}", self.step, self.aggregation, self.price_type)
    }
}
impl FromStr for BarSpecification {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}
impl crate::wire::Wire for BarSpecification {
    fn encode(&self, w: &mut crate::wire::WireWriter) {
        w.u32(self.step);
        w.put(&self.aggregation);
        w.put(&self.price_type);
    }
    fn decode(r: &mut crate::wire::WireReader<'_>) -> Result<Self> {
        Self::new(r.u32()?, r.get()?, r.get()?)
    }
}

/// Standard form `{instrument_id}-{step}-{AGG}-{PRICE_TYPE}-{SOURCE}`; the composite form appends
/// `@{step}-{AGG}-{SOURCE}` naming the bars it is built from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BarType {
    pub instrument_id: InstrumentId,
    pub spec: BarSpecification,
    pub aggregation_source: AggregationSource,
    /// The composite's source bars, if any.
    pub composite: Option<CompositeSource>,
}

/// `@{step}-{AGG}-{SOURCE}` of a composite bar type. The price type is the composite's own.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CompositeSource {
    pub step: u32,
    pub aggregation: BarAggregation,
    pub aggregation_source: AggregationSource,
}
wire_struct!(CompositeSource { step, aggregation, aggregation_source });

impl BarType {
    #[must_use]
    pub const fn standard(
        instrument_id: InstrumentId,
        spec: BarSpecification,
        aggregation_source: AggregationSource,
    ) -> Self {
        Self { instrument_id, spec, aggregation_source, composite: None }
    }

    pub fn parse(text: &str) -> Result<Self> {
        let (standard, composite) = match text.split_once('@') {
            Some((s, c)) => (s, Some(c)),
            None => (text, None),
        };
        // The instrument id may contain '-', so take the last four dash-separated fields.
        let mut fields = standard.rsplitn(5, '-');
        let source = fields.next().ok_or(Status::ParseError)?;
        let price = fields.next().ok_or(Status::ParseError)?;
        let agg = fields.next().ok_or(Status::ParseError)?;
        let step = fields.next().ok_or(Status::ParseError)?;
        let instrument = fields.next().ok_or(Status::ParseError)?;
        let mut spec_text = [0u8; 64];
        let joined = {
            let n = step.len() + 1 + agg.len() + 1 + price.len();
            if n > spec_text.len() {
                return Err(Status::ParseError);
            }
            spec_text[..step.len()].copy_from_slice(step.as_bytes());
            spec_text[step.len()] = b'-';
            spec_text[step.len() + 1..step.len() + 1 + agg.len()].copy_from_slice(agg.as_bytes());
            spec_text[step.len() + 1 + agg.len()] = b'-';
            spec_text[step.len() + 2 + agg.len()..n].copy_from_slice(price.as_bytes());
            core::str::from_utf8(&spec_text[..n]).map_err(|_| Status::ParseError)?
        };
        let mut out = Self::standard(
            InstrumentId::parse(instrument)?,
            BarSpecification::parse(joined)?,
            AggregationSource::from_text(source)?,
        );
        if let Some(c) = composite {
            let mut parts = c.splitn(3, '-');
            let step = parts.next().ok_or(Status::ParseError)?;
            let agg = parts.next().ok_or(Status::ParseError)?;
            let src = parts.next().ok_or(Status::ParseError)?;
            if step.is_empty() || !step.bytes().all(|b| b.is_ascii_digit()) {
                return Err(Status::ParseError);
            }
            let step: u32 = step.parse().map_err(|_| Status::OutOfRange)?;
            if step == 0 {
                return Err(Status::InvalidArgument);
            }
            out.composite = Some(CompositeSource {
                step,
                aggregation: BarAggregation::from_text(agg)?,
                aggregation_source: AggregationSource::from_text(src)?,
            });
        }
        Ok(out)
    }

    #[must_use]
    pub const fn is_composite(&self) -> bool {
        self.composite.is_some()
    }
}
impl fmt::Display for BarType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}-{}-{}", self.instrument_id, self.spec, self.aggregation_source)?;
        if let Some(c) = &self.composite {
            write!(f, "@{}-{}-{}", c.step, c.aggregation, c.aggregation_source)?;
        }
        Ok(())
    }
}
impl FromStr for BarType {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}
wire_struct!(BarType { instrument_id, spec, aggregation_source, composite });

/// OHLCV at `ts_event`; in backtest `ts_init` equals the close time (docs/architecture.md
/// section 5.3).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Bar {
    pub bar_type: BarType,
    pub open: Price,
    pub high: Price,
    pub low: Price,
    pub close: Price,
    pub volume: Quantity,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl Bar {
    /// `InvalidArgument` unless `low <= open, close <= high`, every price is a real value and
    /// `ts_init >= ts_event`.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        bar_type: BarType,
        open: Price,
        high: Price,
        low: Price,
        close: Price,
        volume: Quantity,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if [open, high, low, close].iter().any(|p| p.is_undef()) || volume.is_undef() {
            return Err(Status::InvalidArgument);
        }
        if !(low <= open && open <= high && low <= close && close <= high) || ts_init < ts_event {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { bar_type, open, high, low, close, volume, ts_event, ts_init })
    }
}
impl crate::wire::Wire for Bar {
    fn encode(&self, w: &mut crate::wire::WireWriter) {
        w.put(&self.bar_type);
        w.put(&self.open);
        w.put(&self.high);
        w.put(&self.low);
        w.put(&self.close);
        w.put(&self.volume);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut crate::wire::WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?, r.get()?)
    }
}
