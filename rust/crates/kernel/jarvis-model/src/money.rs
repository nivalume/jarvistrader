//! `Money`: an amount in a currency, fixed-point on the 1e9 scale, shown at the currency's
//! precision.

use core::cmp::Ordering;
use core::fmt;
use core::str::FromStr;

use jarvis_core::int_math::POW10;
use jarvis_core::{Result, Status};

use crate::currency::Currency;
use crate::decimal;
use crate::fixed_point::{FIXED_PRECISION, FIXED_SCALAR};

/// `MONEY_MAX` as a raw value: `9_223_372_036.0`.
pub const MONEY_RAW_MAX: i64 = 9_223_372_036 * FIXED_SCALAR;
pub const MONEY_RAW_MIN: i64 = -MONEY_RAW_MAX;

/// An amount of a currency. Its `raw` carries no digits below the currency's precision: every
/// constructor truncates toward zero, so the system never credits an amount that rounding
/// invented (docs/architecture.md section 6.1).
#[derive(Clone, Copy)]
pub struct Money {
    raw: i64,
    currency: Currency,
}

impl Money {
    /// `raw` on the 1e9 scale, truncated toward zero to the currency's precision; `OutOfRange`
    /// outside `[MONEY_RAW_MIN, MONEY_RAW_MAX]`.
    pub fn from_raw(raw: i64, currency: Currency) -> Result<Self> {
        if !(MONEY_RAW_MIN..=MONEY_RAW_MAX).contains(&raw) {
            return Err(Status::OutOfRange);
        }
        let step = POW10[(FIXED_PRECISION - currency.precision) as usize] as i64;
        Ok(Self { raw: raw - raw % step, currency })
    }

    /// `raw` that must already be exact at the currency's precision (`InvalidArgument` otherwise);
    /// decoders use it so that a non-canonical encoding is refused rather than repaired.
    pub fn from_raw_exact(raw: i64, currency: Currency) -> Result<Self> {
        let m = Self::from_raw(raw, currency)?;
        if m.raw != raw {
            return Err(Status::InvalidArgument);
        }
        Ok(m)
    }

    /// Parses `"{amount} {CODE}"`; the amount is rounded half to even to the currency's precision.
    pub fn parse(text: &str) -> Result<Self> {
        let (amount, code) = text.split_once(' ').ok_or(Status::ParseError)?;
        let currency = Currency::builtin_by_code(code).ok_or(Status::NotFound)?;
        Self::parse_amount(amount, currency)
    }

    /// Parses the amount alone for a known currency.
    pub fn parse_amount(amount: &str, currency: Currency) -> Result<Self> {
        let d = decimal::parse(amount)?;
        let raw = decimal::to_raw(&d, currency.precision)?;
        let raw = i64::try_from(raw).map_err(|_| Status::OutOfRange)?;
        Self::from_raw(raw, currency)
    }

    #[must_use]
    pub const fn zero(currency: Currency) -> Self {
        Self { raw: 0, currency }
    }
    #[must_use]
    pub const fn raw(&self) -> i64 {
        self.raw
    }
    #[must_use]
    pub const fn currency(&self) -> Currency {
        self.currency
    }
    #[must_use]
    pub const fn is_zero(&self) -> bool {
        self.raw == 0
    }
    #[must_use]
    pub const fn is_negative(&self) -> bool {
        self.raw < 0
    }

    /// `InvalidArgument` when the currencies differ, `Overflow` past the range.
    pub fn checked_add(self, other: Money) -> Result<Money> {
        self.combine(other, i64::checked_add)
    }
    pub fn checked_sub(self, other: Money) -> Result<Money> {
        self.combine(other, i64::checked_sub)
    }
    fn combine(self, other: Money, op: fn(i64, i64) -> Option<i64>) -> Result<Money> {
        if self.currency != other.currency {
            return Err(Status::InvalidArgument);
        }
        let raw = op(self.raw, other.raw).ok_or(Status::Overflow)?;
        Self::from_raw(raw, self.currency).map_err(|_| Status::Overflow)
    }
    #[must_use]
    pub fn negated(self) -> Money {
        Self { raw: -self.raw, currency: self.currency }
    }
}

impl PartialEq for Money {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw && self.currency == other.currency
    }
}
impl Eq for Money {}
/// Order by currency code first, then amount; amounts in different currencies are not
/// comparable as values, but the total order keeps balances sortable.
impl PartialOrd for Money {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Money {
    fn cmp(&self, other: &Self) -> Ordering {
        self.currency.cmp(&other.currency).then(self.raw.cmp(&other.raw))
    }
}
impl core::hash::Hash for Money {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.raw.hash(state);
        self.currency.hash(state);
    }
}
impl Default for Money {
    fn default() -> Self {
        Self::zero(Currency::default())
    }
}
impl fmt::Display for Money {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        decimal::fmt_raw(f, i128::from(self.raw), self.currency.precision)?;
        write!(f, " {}", self.currency)
    }
}
impl fmt::Debug for Money {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Money({self})")
    }
}
impl FromStr for Money {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}
