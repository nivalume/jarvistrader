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
    pub const fn plus(self, d: DurationNanos) -> Result<UnixNanos> {
        match self.0.checked_add(d.0) {
            Some(sum) => Ok(UnixNanos(sum)),
            None => Err(Status::Overflow),
        }
    }

    /// Duration from `earlier` to this instant; `OutOfRange` if `earlier` is later.
    pub const fn since(self, earlier: UnixNanos) -> Result<DurationNanos> {
        if earlier.0 > self.0 {
            return Err(Status::OutOfRange);
        }
        Ok(DurationNanos(self.0 - earlier.0))
    }
}

pub const NANOS_PER_SECOND: u64 = 1_000_000_000;
pub const SECONDS_PER_DAY: u64 = 86_400;
/// `"YYYY-MM-DDTHH:MM:SS.fffffffff+00:00"`
pub const RFC3339_MAX_LENGTH: usize = 35;

/// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant, "chrono-compatible
/// low-level date algorithms").
#[must_use]
pub const fn days_from_civil(year: i64, month: u32, day: u32) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = (if year >= 0 { year } else { year - 399 }) / 400;
    let yoe = (year - era * 400) as u64;
    let mp = if month > 2 { month - 3 } else { month + 9 } as u64;
    let doy = (153 * mp + 2) / 5 + day as u64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe as i64 - 719_468
}

/// A proleptic Gregorian calendar date.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CivilDate {
    pub year: i64,
    pub month: u32,
    pub day: u32,
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

const fn put_digits(out: &mut [u8], pos: &mut usize, mut value: u64, width: usize) {
    let mut i = width;
    while i > 0 {
        out[*pos + i - 1] = b'0' + (value % 10) as u8;
        value /= 10;
        i -= 1;
    }
    *pos += width;
}

const fn read_digits(text: &[u8], pos: &mut usize, width: usize) -> Option<u64> {
    if *pos + width > text.len() {
        return None;
    }
    let mut value: u64 = 0;
    let mut i = 0;
    while i < width {
        let c = text[*pos + i];
        if !c.is_ascii_digit() {
            return None;
        }
        value = value * 10 + (c - b'0') as u64;
        i += 1;
    }
    *pos += width;
    Some(value)
}

const fn expect(text: &[u8], pos: &mut usize, c: u8) -> bool {
    if *pos >= text.len() || text[*pos] != c {
        return false;
    }
    *pos += 1;
    true
}

/// Formats like nautilus `UnixNanos::to_rfc3339`: `+00:00` offset and 0, 3, 6 or 9 fractional
/// digits, whichever shows every non-zero digit. Returns the number of bytes written; `out` needs
/// [`RFC3339_MAX_LENGTH`] bytes.
pub const fn format_rfc3339(t: UnixNanos, out: &mut [u8]) -> Result<usize> {
    if out.len() < RFC3339_MAX_LENGTH {
        return Err(Status::OutOfRange);
    }
    let seconds = t.0 / NANOS_PER_SECOND;
    let nanos = t.0 % NANOS_PER_SECOND;
    let days = seconds / SECONDS_PER_DAY;
    let secs_of_day = seconds % SECONDS_PER_DAY;
    let date = civil_from_days(days as i64);

    let mut pos = 0;
    put_digits(out, &mut pos, date.year as u64, 4);
    out[pos] = b'-';
    pos += 1;
    put_digits(out, &mut pos, date.month as u64, 2);
    out[pos] = b'-';
    pos += 1;
    put_digits(out, &mut pos, date.day as u64, 2);
    out[pos] = b'T';
    pos += 1;
    put_digits(out, &mut pos, secs_of_day / 3600, 2);
    out[pos] = b':';
    pos += 1;
    put_digits(out, &mut pos, secs_of_day / 60 % 60, 2);
    out[pos] = b':';
    pos += 1;
    put_digits(out, &mut pos, secs_of_day % 60, 2);
    if nanos != 0 {
        out[pos] = b'.';
        pos += 1;
        if nanos % 1_000_000 == 0 {
            put_digits(out, &mut pos, nanos / 1_000_000, 3);
        } else if nanos % 1_000 == 0 {
            put_digits(out, &mut pos, nanos / 1_000, 6);
        } else {
            put_digits(out, &mut pos, nanos, 9);
        }
    }
    let suffix = b"+00:00";
    let mut i = 0;
    while i < suffix.len() {
        out[pos] = suffix[i];
        pos += 1;
        i += 1;
    }
    Ok(pos)
}

/// `"YYYY-MM-DD(T|t| )HH:MM:SS"` at `pos`: days since the epoch and seconds of the day.
const fn parse_date_time(text: &[u8], pos: &mut usize) -> Result<(i64, u64)> {
    let Some(year) = read_digits(text, pos, 4) else { return Err(Status::ParseError) };
    if !expect(text, pos, b'-') {
        return Err(Status::ParseError);
    }
    let Some(month) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if !expect(text, pos, b'-') {
        return Err(Status::ParseError);
    }
    let Some(day) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if *pos >= text.len() || (text[*pos] != b'T' && text[*pos] != b't' && text[*pos] != b' ') {
        return Err(Status::ParseError);
    }
    *pos += 1;
    let Some(hour) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if !expect(text, pos, b':') {
        return Err(Status::ParseError);
    }
    let Some(minute) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if !expect(text, pos, b':') {
        return Err(Status::ParseError);
    }
    let Some(second) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59 {
        return Err(Status::OutOfRange);
    }
    let days = days_from_civil(year as i64, month as u32, day as u32);
    let roundtrip = civil_from_days(days);
    if roundtrip.month as u64 != month || roundtrip.day as u64 != day {
        return Err(Status::OutOfRange); // e.g. February 30
    }
    Ok((days, hour * 3600 + minute * 60 + second))
}

/// Optional `".f{1,9}"` at `pos`, as nanoseconds.
const fn parse_fraction(text: &[u8], pos: &mut usize) -> Result<u64> {
    if *pos >= text.len() || text[*pos] != b'.' {
        return Ok(0);
    }
    *pos += 1;
    let mut nanos: u64 = 0;
    let mut digits = 0;
    while *pos < text.len() && text[*pos].is_ascii_digit() {
        if digits == 9 {
            return Err(Status::PrecisionLoss);
        }
        nanos = nanos * 10 + (text[*pos] - b'0') as u64;
        digits += 1;
        *pos += 1;
    }
    if digits == 0 {
        return Err(Status::ParseError);
    }
    while digits < 9 {
        nanos *= 10;
        digits += 1;
    }
    Ok(nanos)
}

/// `"Z"`, `"z"`, `"+HH:MM"` or `"-HH:MM"` at `pos`, as seconds east of UTC.
const fn parse_offset(text: &[u8], pos: &mut usize) -> Result<i64> {
    if *pos < text.len() && (text[*pos] == b'Z' || text[*pos] == b'z') {
        *pos += 1;
        return Ok(0);
    }
    if *pos >= text.len() || (text[*pos] != b'+' && text[*pos] != b'-') {
        return Err(Status::ParseError);
    }
    let negative = text[*pos] == b'-';
    *pos += 1;
    let Some(oh) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if !expect(text, pos, b':') {
        return Err(Status::ParseError);
    }
    let Some(om) = read_digits(text, pos, 2) else { return Err(Status::ParseError) };
    if oh > 23 || om > 59 {
        return Err(Status::ParseError);
    }
    let seconds = (oh * 3600 + om * 60) as i64;
    Ok(if negative { -seconds } else { seconds })
}

/// Parses `"YYYY-MM-DDTHH:MM:SS[.f{1,9}](Z|+HH:MM|-HH:MM)"`; the separator may also be `t` or a
/// space, as RFC 3339 section 5.6 allows. Instants before the epoch or past the range of
/// [`UnixNanos`] are `OutOfRange`.
pub const fn parse_rfc3339(text: &str) -> Result<UnixNanos> {
    let text = text.as_bytes();
    let mut pos = 0;
    let (days, secs_of_day) = match parse_date_time(text, &mut pos) {
        Ok(v) => v,
        Err(s) => return Err(s),
    };
    let nanos = match parse_fraction(text, &mut pos) {
        Ok(v) => v,
        Err(s) => return Err(s),
    };
    let offset_seconds = match parse_offset(text, &mut pos) {
        Ok(v) => v,
        Err(s) => return Err(s),
    };
    if pos != text.len() {
        return Err(Status::ParseError);
    }
    let total_seconds = days * SECONDS_PER_DAY as i64 + secs_of_day as i64 - offset_seconds;
    if total_seconds < 0 {
        return Err(Status::OutOfRange);
    }
    let Some(ns) = (total_seconds as u64).checked_mul(NANOS_PER_SECOND) else {
        return Err(Status::OutOfRange);
    };
    match ns.checked_add(nanos) {
        Some(ns) => Ok(UnixNanos(ns)),
        None => Err(Status::OutOfRange),
    }
}
