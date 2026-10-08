//! Decimal text to and from the fixed-point `raw` on the 1e9 scale.
//!
//! Parsing accepts `[+-]digits[.digits][(e|E)[+-]digits]`; nothing else, no whitespace, no
//! separators. Precision is the number of fractional digits after the exponent is applied. A
//! value with more than nine fractional digits is `PrecisionLoss` when the caller wants the
//! precision inferred, and is rounded half to even when the caller names a precision.

use kernel_core::int_math::POW10;
use kernel_core::{Result, Status};

use crate::fixed_point::FIXED_PRECISION;

/// A parsed decimal: `(-1)^negative * mantissa * 10^-scale`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Decimal {
    pub negative: bool,
    pub mantissa: u128,
    pub scale: u32,
}

const MAX_DIGITS: usize = 38; // u128 holds 38 full decimal digits

/// Parses the text form. The mantissa keeps every digit written (leading zeros dropped), so the
/// result is exact; `ParseError` for a malformed string, `Overflow` past 38 significant digits or
/// an exponent that does not fit.
pub fn parse(text: &str) -> Result<Decimal> {
    let b = text.as_bytes();
    let mut pos = 0;
    let negative = match b.first() {
        Some(b'-') => {
            pos += 1;
            true
        }
        Some(b'+') => {
            pos += 1;
            false
        }
        _ => false,
    };
    let mut mantissa: u128 = 0;
    let mut digits = 0usize;
    let mut significant = 0usize;
    let mut fraction_digits: i64 = 0;
    let mut seen_point = false;
    while pos < b.len() {
        let c = b[pos];
        if c.is_ascii_digit() {
            if significant > 0 || c != b'0' {
                significant += 1;
                if significant > MAX_DIGITS {
                    return Err(Status::Overflow);
                }
                mantissa = mantissa * 10 + u128::from(c - b'0');
            }
            if seen_point {
                fraction_digits += 1;
            }
            digits += 1;
        } else if c == b'.' && !seen_point {
            seen_point = true;
        } else {
            break;
        }
        pos += 1;
    }
    if digits == 0 {
        return Err(Status::ParseError);
    }
    let mut exponent: i64 = 0;
    if pos < b.len() && (b[pos] == b'e' || b[pos] == b'E') {
        pos += 1;
        let exp_negative = match b.get(pos) {
            Some(b'-') => {
                pos += 1;
                true
            }
            Some(b'+') => {
                pos += 1;
                false
            }
            _ => false,
        };
        let start = pos;
        while pos < b.len() && b[pos].is_ascii_digit() {
            exponent = exponent.checked_mul(10).ok_or(Status::Overflow)? + i64::from(b[pos] - b'0');
            if exponent > 10_000 {
                return Err(Status::Overflow);
            }
            pos += 1;
        }
        if pos == start {
            return Err(Status::ParseError);
        }
        if exp_negative {
            exponent = -exponent;
        }
    }
    if pos != b.len() {
        return Err(Status::ParseError);
    }
    let scale = fraction_digits - exponent;
    if scale < 0 {
        // Shift the mantissa left; a negative scale is the same number with trailing zeros.
        let shift = (-scale) as u32;
        if shift as usize > MAX_DIGITS {
            return Err(Status::Overflow);
        }
        mantissa = mantissa.checked_mul(pow10_u128(shift)).ok_or(Status::Overflow)?;
        return Ok(Decimal { negative, mantissa, scale: 0 });
    }
    Ok(Decimal { negative, mantissa, scale: scale as u32 })
}

const fn pow10_u128(n: u32) -> u128 {
    let mut v: u128 = 1;
    let mut i = 0;
    while i < n {
        v *= 10;
        i += 1;
    }
    v
}

/// The precision a text form implies: the fractional digits written, after the exponent is
/// applied. `"1.230"` is precision 3 and `"1.23"` precision 2; they are equal values.
#[must_use]
pub const fn inferred_precision(d: &Decimal) -> u32 {
    d.scale
}

/// The value on the 1e9 scale, rounded half to even to `precision` fractional digits (which must
/// not exceed nine). `Overflow` when the result does not fit in `i128` range the caller checks.
pub fn to_raw(d: &Decimal, precision: u8) -> Result<i128> {
    if precision > FIXED_PRECISION {
        return Err(Status::InvalidArgument);
    }
    // First to `precision` digits with rounding, then up to the 1e9 scale exactly.
    let keep = u32::from(precision);
    let quantized: u128 = if d.scale <= keep {
        d.mantissa.checked_mul(pow10_u128(keep - d.scale)).ok_or(Status::Overflow)?
    } else {
        let drop = d.scale - keep;
        if drop as usize > MAX_DIGITS {
            if d.mantissa == 0 {
                0
            } else {
                // Everything is below half of the last kept digit, except we cannot tell a tie:
                // with more than 38 dropped digits the mantissa is at most 10^38 < 10^drop / 2.
                0
            }
        } else {
            let divisor = pow10_u128(drop);
            let q = d.mantissa / divisor;
            let r = d.mantissa % divisor;
            let half = divisor / 2;
            if r > half || (r == half && q % 2 == 1) {
                q + 1
            } else {
                q
            }
        }
    };
    let raw = quantized
        .checked_mul(pow10_u128(u32::from(FIXED_PRECISION) - keep))
        .ok_or(Status::Overflow)?;
    if raw > i128::MAX as u128 {
        return Err(Status::Overflow);
    }
    let raw = raw as i128;
    Ok(if d.negative { -raw } else { raw })
}

/// Writes `raw` (on the 1e9 scale) with exactly `precision` fractional digits. Digits below the
/// precision are truncated; constructors keep them zero, so nothing is lost. Returns the length.
/// `out` needs [`MAX_TEXT`] bytes.
pub fn format_raw(raw: i128, precision: u8, out: &mut [u8]) -> usize {
    let negative = raw < 0;
    let magnitude = raw.unsigned_abs();
    let integral = magnitude / u128::from(POW10[FIXED_PRECISION as usize]);
    let fraction = magnitude % u128::from(POW10[FIXED_PRECISION as usize]);
    let mut pos = 0;
    if negative {
        out[pos] = b'-';
        pos += 1;
    }
    pos += write_u128(integral, &mut out[pos..]);
    if precision > 0 {
        out[pos] = b'.';
        pos += 1;
        let shown = fraction / u128::from(POW10[(FIXED_PRECISION - precision) as usize]);
        let width = precision as usize;
        let mut v = shown;
        for i in (0..width).rev() {
            out[pos + i] = b'0' + (v % 10) as u8;
            v /= 10;
        }
        pos += width;
    }
    pos
}

/// Longest text [`format_raw`] writes: sign, 30 integral digits, point, 9 fractional digits.
pub const MAX_TEXT: usize = 1 + 30 + 1 + 9;

fn write_u128(mut v: u128, out: &mut [u8]) -> usize {
    if v == 0 {
        out[0] = b'0';
        return 1;
    }
    let mut digits = 0;
    let mut t = v;
    while t > 0 {
        digits += 1;
        t /= 10;
    }
    for i in (0..digits).rev() {
        out[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    digits
}

/// Convenience for `Display` impls: formats into a stack buffer and hands the text to `f`.
pub fn fmt_raw(f: &mut core::fmt::Formatter<'_>, raw: i128, precision: u8) -> core::fmt::Result {
    let mut buf = [0u8; MAX_TEXT];
    let n = format_raw(raw, precision, &mut buf);
    f.write_str(core::str::from_utf8(&buf[..n]).unwrap_or("?"))
}
