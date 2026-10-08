//! Exact integer arithmetic for fixed-point values: `floor(a * b * c / d)` through a 192-bit
//! intermediate, its ceiling, the signed variant, and an integer square root.

use crate::status::{Result, Status};

/// Powers of ten up to `10^19`, the largest that fits in a `u64`.
pub const POW10: [u64; 20] = {
    let mut table = [1u64; 20];
    let mut i = 1;
    while i < 20 {
        table[i] = table[i - 1] * 10;
        i += 1;
    }
    table
};

/// A 192-bit unsigned product, as `hi * 2^128 + lo`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct U192 {
    hi: u64,
    lo: u128,
}

impl U192 {
    /// `x * y` exactly.
    fn mul(x: u128, y: u64) -> Self {
        let y = u128::from(y);
        let p_lo = (x & u128::from(u64::MAX)) * y; // < 2^128
        let p_hi = (x >> 64) * y; // < 2^128
        let (lo, carry) = p_lo.overflowing_add(p_hi << 64);
        Self { hi: ((p_hi >> 64) as u64) + u64::from(carry), lo }
    }

    /// Division by a non-zero 64-bit divisor: `(quotient, remainder)`, the quotient truncated.
    fn div_rem(self, d: u64) -> (Self, u64) {
        let d = u128::from(d);
        let q_hi = u128::from(self.hi) / d;
        let r_hi = u128::from(self.hi) % d;
        let mid = (r_hi << 64) | (self.lo >> 64);
        let q_mid = mid / d;
        let r_mid = mid % d;
        let low = (r_mid << 64) | (self.lo & u128::from(u64::MAX));
        let q_lo = low / d;
        let r = (low % d) as u64;
        (Self { hi: q_hi as u64, lo: (q_mid << 64) | q_lo }, r)
    }
}

/// `floor(a * b * c / d)`, computed exactly through a 192-bit intermediate.
///
/// `Overflow` when the quotient does not fit in 64 bits, `InvalidArgument` when `d` is zero.
pub fn mul_div_u64(a: u64, b: u64, c: u64, d: u64) -> Result<u64> {
    mul_div_rem(a, b, c, d).map(|(q, _)| q)
}

/// `ceil(a * b * c / d)`, exactly. `ceil(ceil(x / p) / q) == ceil(x / (p * q))`, so a caller can
/// round up in stages without losing the remainder of the first division.
pub fn mul_div_u64_up(a: u64, b: u64, c: u64, d: u64) -> Result<u64> {
    let (q, r) = mul_div_rem(a, b, c, d)?;
    if r == 0 {
        return Ok(q);
    }
    q.checked_add(1).ok_or(Status::Overflow)
}

fn mul_div_rem(a: u64, b: u64, c: u64, d: u64) -> Result<(u64, u64)> {
    if d == 0 {
        return Err(Status::InvalidArgument);
    }
    let (q, r) = U192::mul(u128::from(a) * u128::from(b), c).div_rem(d);
    if q.hi != 0 || q.lo > u128::from(u64::MAX) {
        return Err(Status::Overflow);
    }
    Ok((q.lo as u64, r))
}

/// Signed `a * b * c / d`: the magnitude is computed with [`mul_div_u64`] and the sign applied
/// afterwards, so the result truncates toward zero. `Overflow` if it does not fit in `i64`.
pub fn mul_div_i64(a: i64, b: i64, c: i64, d: i64) -> Result<i64> {
    let negative = [a, b, c, d].iter().filter(|v| v.is_negative()).count() % 2 == 1;
    let q = mul_div_u64(a.unsigned_abs(), b.unsigned_abs(), c.unsigned_abs(), d.unsigned_abs())?;
    if negative {
        // -2^63 is representable; q == 2^63 maps to it.
        0i64.checked_sub_unsigned(q).ok_or(Status::Overflow)
    } else {
        i64::try_from(q).map_err(|_| Status::Overflow)
    }
}

/// `floor(sqrt(v))`, exact.
#[must_use]
pub const fn isqrt(v: u128) -> u64 {
    // Newton's method from an upper bound; converges in a few steps and never undershoots the
    // true root on the way down, so the loop stops at floor(sqrt(v)).
    if v == 0 {
        return 0;
    }
    let mut x: u128 = 1 << (128 - v.leading_zeros()).div_ceil(2);
    loop {
        let y = x.midpoint(v / x);
        if y >= x {
            return x as u64;
        }
        x = y;
    }
}
