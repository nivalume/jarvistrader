//! Instants and durations in nanoseconds, civil date arithmetic, and RFC 3339 text.

use crate::status::{Result, Status};

/// A span of time in nanoseconds.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DurationNanos(u64);

impl DurationNanos {
    #[must_use]
    pub const fn new(ns: u64) -> Self {
        Self(ns)
    }
    #[must_use]
    pub const fn from_millis(ms: u64) -> Self {
        Self(ms * 1_000_000)
    }
    #[must_use]
    pub const fn from_secs(s: u64) -> Self {
        Self(s * NANOS_PER_SECOND)
    }
    #[must_use]
    pub const fn value(self) -> u64 {
        self.0
    }
    #[must_use]
    pub const fn is_zero(self) -> bool {
        self.0 == 0
    }
}

/// Nanoseconds since the Unix epoch, UTC (nautilus `UnixNanos`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct UnixNanos(u64);

impl UnixNanos {
    #[must_use]
    pub const fn new(ns: u64) -> Self {
        Self(ns)
    }
    #[must_use]
    pub const fn value(self) -> u64 {
        self.0
    }

    /// This instant plus `d`; `Overflow` past the range of `u64`.
    pub fn plus(self, d: DurationNanos) -> Result<UnixNanos> {
        self.0.checked_add(d.0).map(UnixNanos).ok_or(Status::Overflow)
    }

    /// Duration from `earlier` to this instant; `OutOfRange` if `earlier` is later.
    pub fn since(self, earlier: UnixNanos) -> Result<DurationNanos> {
        self.0.checked_sub(earlier.0).map(DurationNanos).ok_or(Status::OutOfRange)
    }

    /// Formats as RFC 3339 into a stack buffer; see [`format_rfc3339`].
    #[must_use]
    pub fn to_rfc3339(self) -> Rfc3339 {
        let mut text = Rfc3339 { buf: [0; RFC3339_MAX_LENGTH], len: 0 };
        text.len = format_rfc3339(self, &mut text.buf).unwrap_or(0);
        text
    }
}

/// RFC 3339 text on the stack.
#[derive(Clone, Copy, Debug)]
pub struct Rfc3339 {
    buf: [u8; RFC3339_MAX_LENGTH],
    len: usize,
}
impl Rfc3339 {
    #[must_use]
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }
}
impl core::fmt::Display for Rfc3339 {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.as_str())
    }
}
impl core::fmt::Display for UnixNanos {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.to_rfc3339().as_str())
    }
}

pub const NANOS_PER_SECOND: u64 = 1_000_000_000;
pub const SECONDS_PER_DAY: u64 = 86_400;
/// `"YYYY-MM-DDTHH:MM:SS.fffffffff+00:00"`
pub const RFC3339_MAX_LENGTH: usize = 35;

/// A proleptic Gregorian calendar date.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CivilDate {
    pub year: i64,
    pub month: u32,
    pub day: u32,
}

/// Days since 1970-01-01 (H. Hinnant, "chrono-compatible low-level date algorithms").
#[must_use]
pub const fn days_from_civil(year: i64, month: u32, day: u32) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = (if year >= 0 { year } else { year - 399 }) / 400;
    let yoe = (year - era * 400) as u64;
    let mp = (if month > 2 { month - 3 } else { month + 9 }) as u64;
    let doy = (153 * mp + 2) / 5 + day as u64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe as i64 - 719_468
}

/// The inverse of [`days_from_civil`].
#[must_use]
pub const fn civil_from_days(days: i64) -> CivilDate {
    let days = days + 719_468;
    let era = (if days >= 0 { days } else { days - 146_096 }) / 146_097;
    let doe = (days - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    let year = yoe as i64 + era * 400 + if month <= 2 { 1 } else { 0 };
    CivilDate { year, month, day }
}

/// Appends `value` as exactly `width` decimal digits.
struct Out<'a> {
    buf: &'a mut [u8],
    pos: usize,
}
impl Out<'_> {
    fn digits(&mut self, mut value: u64, width: usize) {
        for slot in self.buf[self.pos..self.pos + width].iter_mut().rev() {
            *slot = b'0' + (value % 10) as u8;
            value /= 10;
        }
        self.pos += width;
    }
    fn byte(&mut self, b: u8) {
        self.buf[self.pos] = b;
        self.pos += 1;
    }
    fn bytes(&mut self, b: &[u8]) {
        self.buf[self.pos..self.pos + b.len()].copy_from_slice(b);
        self.pos += b.len();
    }
}

/// Formats like nautilus `UnixNanos::to_rfc3339`: `+00:00` offset and 0, 3, 6 or 9 fractional
/// digits, whichever shows every non-zero digit. Returns the number of bytes written; `out` needs
/// [`RFC3339_MAX_LENGTH`] bytes.
pub fn format_rfc3339(t: UnixNanos, out: &mut [u8]) -> Result<usize> {
    if out.len() < RFC3339_MAX_LENGTH {
        return Err(Status::OutOfRange);
    }
    let seconds = t.0 / NANOS_PER_SECOND;
    let nanos = t.0 % NANOS_PER_SECOND;
    let date = civil_from_days((seconds / SECONDS_PER_DAY) as i64);
    let secs_of_day = seconds % SECONDS_PER_DAY;

    let mut o = Out { buf: out, pos: 0 };
    o.digits(date.year as u64, 4);
    o.byte(b'-');
    o.digits(u64::from(date.month), 2);
    o.byte(b'-');
    o.digits(u64::from(date.day), 2);
    o.byte(b'T');
    o.digits(secs_of_day / 3600, 2);
    o.byte(b':');
    o.digits(secs_of_day / 60 % 60, 2);
    o.byte(b':');
    o.digits(secs_of_day % 60, 2);
    if nanos != 0 {
        o.byte(b'.');
        match nanos {
            n if n % 1_000_000 == 0 => o.digits(n / 1_000_000, 3),
            n if n % 1_000 == 0 => o.digits(n / 1_000, 6),
            n => o.digits(n, 9),
        }
    }
    o.bytes(b"+00:00");
    Ok(o.pos)
}

/// Cursor over the text being parsed.
struct In<'a> {
    bytes: &'a [u8],
    pos: usize,
}
impl In<'_> {
    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }
    /// Exactly `width` ASCII digits.
    fn digits(&mut self, width: usize) -> Result<u64> {
        let chunk = self.bytes.get(self.pos..self.pos + width).ok_or(Status::ParseError)?;
        let mut value = 0u64;
        for &c in chunk {
            if !c.is_ascii_digit() {
                return Err(Status::ParseError);
            }
            value = value * 10 + u64::from(c - b'0');
        }
        self.pos += width;
        Ok(value)
    }
    fn expect(&mut self, c: u8) -> Result<()> {
        if self.peek() != Some(c) {
            return Err(Status::ParseError);
        }
        self.pos += 1;
        Ok(())
    }
    fn take_if(&mut self, accept: impl Fn(u8) -> bool) -> Option<u8> {
        let c = self.peek().filter(|&c| accept(c))?;
        self.pos += 1;
        Some(c)
    }
}

/// Parses `"YYYY-MM-DDTHH:MM:SS[.f{1,9}](Z|+HH:MM|-HH:MM)"`; the separator may also be `t` or a
/// space, as RFC 3339 section 5.6 allows. Instants before the epoch or past the range of
/// [`UnixNanos`] are `OutOfRange`; more than nine fractional digits is `PrecisionLoss`.
pub fn parse_rfc3339(text: &str) -> Result<UnixNanos> {
    let mut i = In { bytes: text.as_bytes(), pos: 0 };
    let year = i.digits(4)?;
    i.expect(b'-')?;
    let month = i.digits(2)?;
    i.expect(b'-')?;
    let day = i.digits(2)?;
    i.take_if(|c| matches!(c, b'T' | b't' | b' ')).ok_or(Status::ParseError)?;
    let hour = i.digits(2)?;
    i.expect(b':')?;
    let minute = i.digits(2)?;
    i.expect(b':')?;
    let second = i.digits(2)?;
    if !(1..=12).contains(&month)
        || !(1..=31).contains(&day)
        || hour > 23
        || minute > 59
        || second > 59
    {
        return Err(Status::OutOfRange);
    }
    let days = days_from_civil(year as i64, month as u32, day as u32);
    let back = civil_from_days(days);
    if (u64::from(back.month), u64::from(back.day)) != (month, day) {
        return Err(Status::OutOfRange); // e.g. February 30
    }

    let mut nanos = 0u64;
    if i.take_if(|c| c == b'.').is_some() {
        let mut digits = 0;
        while let Some(c) = i.take_if(|c| c.is_ascii_digit()) {
            if digits == 9 {
                return Err(Status::PrecisionLoss);
            }
            nanos = nanos * 10 + u64::from(c - b'0');
            digits += 1;
        }
        if digits == 0 {
            return Err(Status::ParseError);
        }
        nanos *= 10u64.pow(9 - digits);
    }

    let offset_seconds: i64 = match i.take_if(|c| matches!(c, b'Z' | b'z' | b'+' | b'-')) {
        Some(b'Z' | b'z') => 0,
        Some(sign) => {
            let oh = i.digits(2)?;
            i.expect(b':')?;
            let om = i.digits(2)?;
            if oh > 23 || om > 59 {
                return Err(Status::ParseError);
            }
            let seconds = (oh * 3600 + om * 60) as i64;
            if sign == b'-' {
                -seconds
            } else {
                seconds
            }
        }
        None => return Err(Status::ParseError),
    };
    if i.pos != i.bytes.len() {
        return Err(Status::ParseError);
    }

    let total_seconds = days * SECONDS_PER_DAY as i64 + (hour * 3600 + minute * 60 + second) as i64
        - offset_seconds;
    let seconds = u64::try_from(total_seconds).map_err(|_| Status::OutOfRange)?;
    seconds
        .checked_mul(NANOS_PER_SECOND)
        .and_then(|ns| ns.checked_add(nanos))
        .map(UnixNanos)
        .ok_or(Status::OutOfRange)
}

impl core::str::FromStr for UnixNanos {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        parse_rfc3339(s)
    }
}
