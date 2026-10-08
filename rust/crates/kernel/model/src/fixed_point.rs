//! `Price` and `Quantity`: fixed-point values whose `raw` is always on the global 1e9 scale and
//! whose `precision` only affects text (docs/architecture.md section 6.1).
//!
//! Equality and order look at `raw` only, so `Price(1.23, 2) == Price(1.230, 3)`. A constructor
//! refuses a `raw` with digits below its `precision`, so the text form is exact and the invariant
//! "precision is presentation" holds without rounding anywhere later.

use core::cmp::Ordering;
use core::fmt;
use core::str::FromStr;

use kernel_core::int_math::{mul_div_u64, POW10};
use kernel_core::{Result, Status};

use crate::decimal;

pub const FIXED_PRECISION: u8 = 9;
pub const FIXED_SCALAR: i64 = 1_000_000_000;

/// `PRICE_MAX` as a raw value: `9_223_372_036.0`.
pub const PRICE_RAW_MAX: i64 = 9_223_372_036 * FIXED_SCALAR;
pub const PRICE_RAW_MIN: i64 = -PRICE_RAW_MAX;
/// Sentinel raws, as nautilus: a price that is not set, and one that failed to compute.
pub const PRICE_RAW_UNDEF: i64 = i64::MAX;
pub const PRICE_RAW_ERROR: i64 = i64::MIN;
/// `QUANTITY_MAX` as a raw value: `18_446_744_073.0`.
pub const QUANTITY_RAW_MAX: u64 = 18_446_744_073 * FIXED_SCALAR as u64;
pub const QUANTITY_RAW_UNDEF: u64 = u64::MAX;

const fn scale_of(precision: u8) -> u64 {
    POW10[(FIXED_PRECISION - precision) as usize]
}

fn check_precision(precision: u8) -> Result<()> {
    if precision > FIXED_PRECISION {
        return Err(Status::InvalidArgument);
    }
    Ok(())
}

/// A price. `raw` is on the 1e9 scale and may be negative (spreads, some rates).
#[derive(Clone, Copy)]
pub struct Price {
    raw: i64,
    precision: u8,
}

impl Price {
    /// `InvalidArgument` for a precision over nine or digits below the precision; `OutOfRange`
    /// outside `[PRICE_RAW_MIN, PRICE_RAW_MAX]`.
    pub fn from_raw(raw: i64, precision: u8) -> Result<Self> {
        check_precision(precision)?;
        if !(PRICE_RAW_MIN..=PRICE_RAW_MAX).contains(&raw) {
            return Err(Status::OutOfRange);
        }
        if raw % scale_of(precision) as i64 != 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { raw, precision })
    }

    /// An integral number of the smallest units at `precision`: `from_units(123, 2)` is `1.23`.
    pub fn from_units(units: i64, precision: u8) -> Result<Self> {
        check_precision(precision)?;
        let raw = units.checked_mul(scale_of(precision) as i64).ok_or(Status::Overflow)?;
        Self::from_raw(raw, precision)
    }

    /// Parses text, inferring the precision from the fractional digits (`PrecisionLoss` past
    /// nine).
    pub fn parse(text: &str) -> Result<Self> {
        let d = decimal::parse(text)?;
        let precision = decimal::inferred_precision(&d);
        if precision > u32::from(FIXED_PRECISION) {
            return Err(Status::PrecisionLoss);
        }
        Self::from_decimal(&d, precision as u8)
    }

    /// Parses text and rounds half to even to `precision`.
    pub fn parse_with_precision(text: &str, precision: u8) -> Result<Self> {
        Self::from_decimal(&decimal::parse(text)?, precision)
    }

    fn from_decimal(d: &decimal::Decimal, precision: u8) -> Result<Self> {
        let raw = decimal::to_raw(d, precision)?;
        let raw = i64::try_from(raw).map_err(|_| Status::OutOfRange)?;
        Self::from_raw(raw, precision)
    }

    /// The unset price. It compares greater than every real price and formats as `UNDEF`.
    #[must_use]
    pub const fn undef() -> Self {
        Self { raw: PRICE_RAW_UNDEF, precision: 0 }
    }
    #[must_use]
    pub const fn is_undef(self) -> bool {
        self.raw == PRICE_RAW_UNDEF
    }
    #[must_use]
    pub const fn zero(precision: u8) -> Self {
        Self { raw: 0, precision }
    }

    #[must_use]
    pub const fn raw(self) -> i64 {
        self.raw
    }
    #[must_use]
    pub const fn precision(self) -> u8 {
        self.precision
    }
    /// The value in smallest units of its precision.
    #[must_use]
    pub const fn units(self) -> i64 {
        self.raw / scale_of(self.precision) as i64
    }
    #[must_use]
    pub const fn is_zero(self) -> bool {
        self.raw == 0
    }
    #[must_use]
    pub const fn is_positive(self) -> bool {
        self.raw > 0
    }

    /// Sum at the larger precision; `Overflow` past the range, `InvalidArgument` on a sentinel.
    pub fn checked_add(self, other: Price) -> Result<Price> {
        self.combine(other, i64::checked_add)
    }
    pub fn checked_sub(self, other: Price) -> Result<Price> {
        self.combine(other, i64::checked_sub)
    }
    fn combine(self, other: Price, op: fn(i64, i64) -> Option<i64>) -> Result<Price> {
        if self.is_undef() || other.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let raw = op(self.raw, other.raw).ok_or(Status::Overflow)?;
        Self::from_raw(raw, self.precision.max(other.precision)).map_err(|e| match e {
            Status::OutOfRange => Status::Overflow,
            other => other,
        })
    }

    /// `price * n`, for an integral multiplier such as a number of ticks.
    pub fn checked_mul_int(self, n: i64) -> Result<Price> {
        if self.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let raw = self.raw.checked_mul(n).ok_or(Status::Overflow)?;
        Self::from_raw(raw, self.precision).map_err(|_| Status::Overflow)
    }
}

impl PartialEq for Price {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}
impl Eq for Price {}
impl PartialOrd for Price {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Price {
    fn cmp(&self, other: &Self) -> Ordering {
        self.raw.cmp(&other.raw)
    }
}
impl core::hash::Hash for Price {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.raw.hash(state);
    }
}
impl Default for Price {
    fn default() -> Self {
        Self::zero(0)
    }
}
impl fmt::Display for Price {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_undef() {
            return f.write_str("UNDEF");
        }
        decimal::fmt_raw(f, i128::from(self.raw), self.precision)
    }
}
impl fmt::Debug for Price {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Price({self})")
    }
}
impl FromStr for Price {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}

/// A quantity. `raw` is on the 1e9 scale and never negative; a signed size is a `(side, Quantity)`
/// pair or the position layer's signed raw.
#[derive(Clone, Copy)]
pub struct Quantity {
    raw: u64,
    precision: u8,
}

impl Quantity {
    pub fn from_raw(raw: u64, precision: u8) -> Result<Self> {
        check_precision(precision)?;
        if raw > QUANTITY_RAW_MAX {
            return Err(Status::OutOfRange);
        }
        if raw % scale_of(precision) != 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { raw, precision })
    }

    /// An integral number of the smallest units at `precision`: `from_units(5, 3)` is `0.005`.
    pub fn from_units(units: u64, precision: u8) -> Result<Self> {
        check_precision(precision)?;
        let raw = units.checked_mul(scale_of(precision)).ok_or(Status::Overflow)?;
        Self::from_raw(raw, precision)
    }

    pub fn parse(text: &str) -> Result<Self> {
        let d = decimal::parse(text)?;
        let precision = decimal::inferred_precision(&d);
        if precision > u32::from(FIXED_PRECISION) {
            return Err(Status::PrecisionLoss);
        }
        Self::from_decimal(&d, precision as u8)
    }

    pub fn parse_with_precision(text: &str, precision: u8) -> Result<Self> {
        Self::from_decimal(&decimal::parse(text)?, precision)
    }

    fn from_decimal(d: &decimal::Decimal, precision: u8) -> Result<Self> {
        let raw = decimal::to_raw(d, precision)?;
        if raw < 0 {
            return Err(Status::OutOfRange);
        }
        let raw = u64::try_from(raw).map_err(|_| Status::OutOfRange)?;
        Self::from_raw(raw, precision)
    }

    #[must_use]
    pub const fn undef() -> Self {
        Self { raw: QUANTITY_RAW_UNDEF, precision: 0 }
    }
    #[must_use]
    pub const fn is_undef(self) -> bool {
        self.raw == QUANTITY_RAW_UNDEF
    }
    #[must_use]
    pub const fn zero(precision: u8) -> Self {
        Self { raw: 0, precision }
    }
    #[must_use]
    pub const fn raw(self) -> u64 {
        self.raw
    }
    #[must_use]
    pub const fn precision(self) -> u8 {
        self.precision
    }
    #[must_use]
    pub const fn units(self) -> u64 {
        self.raw / scale_of(self.precision)
    }
    #[must_use]
    pub const fn is_zero(self) -> bool {
        self.raw == 0
    }

    pub fn checked_add(self, other: Quantity) -> Result<Quantity> {
        if self.is_undef() || other.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let raw = self.raw.checked_add(other.raw).ok_or(Status::Overflow)?;
        Self::from_raw(raw, self.precision.max(other.precision)).map_err(|_| Status::Overflow)
    }
    /// `OutOfRange` when `other` is larger.
    pub fn checked_sub(self, other: Quantity) -> Result<Quantity> {
        if self.is_undef() || other.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let raw = self.raw.checked_sub(other.raw).ok_or(Status::OutOfRange)?;
        Self::from_raw(raw, self.precision.max(other.precision))
    }
}

impl PartialEq for Quantity {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}
impl Eq for Quantity {}
impl PartialOrd for Quantity {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Quantity {
    fn cmp(&self, other: &Self) -> Ordering {
        self.raw.cmp(&other.raw)
    }
}
impl core::hash::Hash for Quantity {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.raw.hash(state);
    }
}
impl Default for Quantity {
    fn default() -> Self {
        Self::zero(0)
    }
}
impl fmt::Display for Quantity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_undef() {
            return f.write_str("UNDEF");
        }
        decimal::fmt_raw(f, i128::from(self.raw), self.precision)
    }
}
impl fmt::Debug for Quantity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Quantity({self})")
    }
}
impl FromStr for Quantity {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}

/// `price * quantity * multiplier`, exact through a 192-bit intermediate, truncated toward zero to
/// the 1e9 scale. The result is a raw amount for the caller's `Money` (which truncates again to
/// its currency's precision). `multiplier` is on the 1e9 scale too (`1.0` is `FIXED_SCALAR`).
/// `Overflow` past `i64`; `InvalidArgument` for a sentinel operand.
pub fn notional_raw(price: Price, quantity: Quantity, multiplier: Quantity) -> Result<i64> {
    if price.is_undef() || quantity.is_undef() || multiplier.is_undef() {
        return Err(Status::InvalidArgument);
    }
    let scale2 = (FIXED_SCALAR as u64) * (FIXED_SCALAR as u64); // 1e18 fits u64
    let magnitude = mul_div_u64(price.raw.unsigned_abs(), quantity.raw, multiplier.raw, scale2)?;
    if price.raw < 0 {
        if magnitude > i64::MAX as u64 + 1 {
            return Err(Status::Overflow);
        }
        Ok(0i64.wrapping_sub_unsigned(magnitude))
    } else {
        i64::try_from(magnitude).map_err(|_| Status::Overflow)
    }
}
