//! Exact integer arithmetic for fixed-point values: `floor(a * b * c / d)` through a 192-bit
//! intermediate, its ceiling, the signed variant, and an integer square root.

use crate::status::{Result, Status};

/// Powers of ten up to `10^19`, the largest that fits in a `u64`.
pub const POW10: [u64; 20] = [
    1,
    10,
    100,
    1_000,
    10_000,
    100_000,
    1_000_000,
    10_000_000,
    100_000_000,
    1_000_000_000,
    10_000_000_000,
    100_000_000_000,
    1_000_000_000_000,
    10_000_000_000_000,
    100_000_000_000_000,
    1_000_000_000_000_000,
    10_000_000_000_000_000,
    100_000_000_000_000_000,
    1_000_000_000_000_000_000,
    10_000_000_000_000_000_000,
];

/// 192-bit unsigned integer, used only as the exact intermediate of [`mul_div_u64`].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct U192 {
    pub lo: u64,
    pub mid: u64,
    pub hi: u64,
}

/// `x * y` exactly.
#[must_use]
pub const fn mul_u128_u64(x: u128, y: u64) -> U192 {
    let x_lo = x as u64;
    let x_hi = (x >> 64) as u64;
    let p0 = (x_lo as u128) * (y as u128);
    let p1 = (x_hi as u128) * (y as u128);
    let mid = (p0 >> 64) + ((p1 as u64) as u128);
    U192 { lo: p0 as u64, mid: mid as u64, hi: ((p1 >> 64) as u64) + ((mid >> 64) as u64) }
}

/// Long division of a 192-bit value by a non-zero 64-bit divisor. The quotient truncates.
#[must_use]
pub const fn div_u192_u64(n: U192, d: u64) -> U192 {
    let d = d as u128;
    let mut cur: u128 = n.hi as u128;
    let hi = (cur / d) as u64;
    let mut rem = cur % d;
    cur = (rem << 64) | (n.mid as u128);
    let mid = (cur / d) as u64;
    rem = cur % d;
    cur = (rem << 64) | (n.lo as u128);
    let lo = (cur / d) as u64;
    U192 { lo, mid, hi }
}

/// `floor(a * b * c / d)`, computed exactly through a 192-bit intermediate.
///
/// `Overflow` when the quotient does not fit in 64 bits, `InvalidArgument` when `d` is zero.
pub const fn mul_div_u64(a: u64, b: u64, c: u64, d: u64) -> Result<u64> {
    if d == 0 {
        return Err(Status::InvalidArgument);
    }
    let product = mul_u128_u64((a as u128) * (b as u128), c);
    let q = div_u192_u64(product, d);
    if q.hi != 0 || q.mid != 0 {
        return Err(Status::Overflow);
    }
    Ok(q.lo)
}

/// `ceil(a * b * c / d)`, exactly. `ceil(ceil(x / p) / q) == ceil(x / (p * q))`, so a caller can
/// round up in stages without losing the remainder of the first division.
pub const fn mul_div_u64_up(a: u64, b: u64, c: u64, d: u64) -> Result<u64> {
    let q = match mul_div_u64(a, b, c, d) {
        Ok(q) => q,
        Err(s) => return Err(s),
    };
    let product = mul_u128_u64((a as u128) * (b as u128), c);
    // q * d <= product < 2^192 and q, d < 2^64, so the product of the two fits in 128 bits.
    let back = (q as u128) * (d as u128);
    let low = ((product.mid as u128) << 64) | (product.lo as u128);
    if product.hi == 0 && low == back {
        return Ok(q);
    }
    if q == u64::MAX {
        return Err(Status::Overflow);
    }
    Ok(q + 1)
}

/// `|v|` as an unsigned value; defined for `i64::MIN` too.
#[must_use]
pub const fn magnitude(v: i64) -> u64 {
    v.unsigned_abs()
}

/// Signed `a * b * c / d`: the magnitude is computed with [`mul_div_u64`] and the sign applied
/// afterwards, so the result truncates toward zero. `Overflow` if it does not fit in `i64`.
pub const fn mul_div_i64(a: i64, b: i64, c: i64, d: i64) -> Result<i64> {
    if d == 0 {
        return Err(Status::InvalidArgument);
    }
    let negatives = (a < 0) as u32 + (b < 0) as u32 + (c < 0) as u32 + (d < 0) as u32;
    let negative = negatives % 2 == 1;
    let q = match mul_div_u64(magnitude(a), magnitude(b), magnitude(c), magnitude(d)) {
        Ok(q) => q,
        Err(s) => return Err(s),
    };
    if negative {
        if q > (i64::MAX as u64) + 1 {
            return Err(Status::Overflow);
        }
        Ok(0u64.wrapping_sub(q) as i64)
    } else {
        if q > i64::MAX as u64 {
            return Err(Status::Overflow);
        }
        Ok(q as i64)
    }
}

/// `floor(sqrt(v))`, exact, by bitwise digit-by-digit extraction.
#[must_use]
pub const fn isqrt(mut v: u128) -> u64 {
    let mut result: u128 = 0;
    let mut bit: u128 = 1 << 126;
    while bit > v {
        bit >>= 2;
    }
    while bit != 0 {
        if v >= result + bit {
            v -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    result as u64
}
