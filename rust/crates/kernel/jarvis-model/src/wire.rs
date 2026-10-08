//! The event log's value encoding: little-endian, fixed-width, field by field, no padding, no
//! pointers, no capacities. Every type's `decode` goes through its validating constructor, so a
//! record that decodes is a valid value, and `encode(decode(bytes)) == bytes` for every record
//! the reader accepts (canonical form; the property tests check it over the corpus).
//!
//! It is distinct from `jarvis_core::state`, the snapshot encoding, which also records
//! capacities and sticky-fails instead of returning errors.

use alloc::vec::Vec;

use jarvis_core::clock::TimerKey;
use jarvis_core::{DurationNanos, EventKey, FixedString, FixedVec, Result, Status, UnixNanos};

/// Appends encodings to a byte vector.
#[derive(Clone, Debug, Default)]
pub struct WireWriter {
    out: Vec<u8>,
}

impl WireWriter {
    #[must_use]
    pub const fn new() -> Self {
        Self { out: Vec::new() }
    }
    #[must_use]
    pub fn with_capacity(n: usize) -> Self {
        Self { out: Vec::with_capacity(n) }
    }
    pub fn bytes(&mut self, b: &[u8]) {
        self.out.extend_from_slice(b);
    }
    pub fn u8(&mut self, v: u8) {
        self.out.push(v);
    }
    pub fn u16(&mut self, v: u16) {
        self.out.extend_from_slice(&v.to_le_bytes());
    }
    pub fn u32(&mut self, v: u32) {
        self.out.extend_from_slice(&v.to_le_bytes());
    }
    pub fn u64(&mut self, v: u64) {
        self.out.extend_from_slice(&v.to_le_bytes());
    }
    pub fn put<T: Wire>(&mut self, v: &T) {
        v.encode(self);
    }
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        &self.out
    }
    #[must_use]
    pub fn len(&self) -> usize {
        self.out.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.out.is_empty()
    }
    pub fn clear(&mut self) {
        self.out.clear();
    }
    #[must_use]
    pub fn into_vec(self) -> Vec<u8> {
        self.out
    }
    /// Overwrites `n` bytes at `at` (a length field written before its contents were known).
    pub fn patch(&mut self, at: usize, bytes: &[u8]) {
        self.out[at..at + bytes.len()].copy_from_slice(bytes);
    }
}

/// Reads encodings from a byte slice; `Truncated` when the input ends early.
#[derive(Debug)]
pub struct WireReader<'a> {
    input: &'a [u8],
    pos: usize,
}

impl<'a> WireReader<'a> {
    #[must_use]
    pub const fn new(input: &'a [u8]) -> Self {
        Self { input, pos: 0 }
    }
    pub fn bytes(&mut self, n: usize) -> Result<&'a [u8]> {
        if n > self.input.len() - self.pos {
            return Err(Status::Truncated);
        }
        let out = &self.input[self.pos..self.pos + n];
        self.pos += n;
        Ok(out)
    }
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.bytes(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16> {
        let b = self.bytes(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }
    pub fn u32(&mut self) -> Result<u32> {
        let b = self.bytes(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    pub fn u64(&mut self) -> Result<u64> {
        let b = self.bytes(8)?;
        Ok(u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
    }
    pub fn get<T: Wire>(&mut self) -> Result<T> {
        T::decode(self)
    }
    #[must_use]
    pub const fn position(&self) -> usize {
        self.pos
    }
    #[must_use]
    pub fn remaining(&self) -> usize {
        self.input.len() - self.pos
    }
    #[must_use]
    pub fn rest(&self) -> &'a [u8] {
        &self.input[self.pos..]
    }
    /// The whole input, for checksums over a range already read.
    #[must_use]
    pub const fn whole(&self) -> &'a [u8] {
        self.input
    }
    /// `InvalidArgument` unless everything was consumed.
    pub fn finish(self) -> Result<()> {
        if self.remaining() != 0 {
            return Err(Status::InvalidArgument);
        }
        Ok(())
    }
}

/// A type with a wire encoding.
pub trait Wire: Sized {
    fn encode(&self, w: &mut WireWriter);
    fn decode(r: &mut WireReader<'_>) -> Result<Self>;

    /// The encoding alone.
    fn to_wire(&self) -> Vec<u8> {
        let mut w = WireWriter::new();
        self.encode(&mut w);
        w.into_vec()
    }
    /// Decodes all of `bytes`; trailing bytes are `InvalidArgument`.
    fn from_wire(bytes: &[u8]) -> Result<Self> {
        let mut r = WireReader::new(bytes);
        let v = Self::decode(&mut r)?;
        r.finish()?;
        Ok(v)
    }
}

/// Implements [`Wire`] for a struct by listing its fields in encoding order. `decode` builds the
/// value with a struct literal, so every field must be listed.
#[macro_export]
macro_rules! wire_struct {
    ($t:ident { $($f:ident),+ $(,)? }) => {
        impl $crate::wire::Wire for $t {
            fn encode(&self, w: &mut $crate::wire::WireWriter) {
                $( $crate::wire::Wire::encode(&self.$f, w); )+
            }
            fn decode(r: &mut $crate::wire::WireReader<'_>) -> ::jarvis_core::Result<Self> {
                Ok($t { $( $f: $crate::wire::Wire::decode(r)?, )+ })
            }
        }
    };
}

macro_rules! wire_int {
    ($t:ty, $w:ident, $r:ident, $u:ty) => {
        impl Wire for $t {
            fn encode(&self, w: &mut WireWriter) {
                w.$w(*self as $u);
            }
            fn decode(r: &mut WireReader<'_>) -> Result<Self> {
                Ok(r.$r()? as $t)
            }
        }
    };
}
wire_int!(u8, u8, u8, u8);
wire_int!(i8, u8, u8, u8);
wire_int!(u16, u16, u16, u16);
wire_int!(i16, u16, u16, u16);
wire_int!(u32, u32, u32, u32);
wire_int!(i32, u32, u32, u32);
wire_int!(u64, u64, u64, u64);
wire_int!(i64, u64, u64, u64);

impl Wire for u128 {
    fn encode(&self, w: &mut WireWriter) {
        w.u64(*self as u64);
        w.u64((*self >> 64) as u64);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let lo = r.u64()?;
        let hi = r.u64()?;
        Ok((u128::from(hi) << 64) | u128::from(lo))
    }
}
impl Wire for i128 {
    fn encode(&self, w: &mut WireWriter) {
        (*self as u128).encode(w);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Ok(u128::decode(r)? as i128)
    }
}
impl Wire for bool {
    fn encode(&self, w: &mut WireWriter) {
        w.u8(u8::from(*self));
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        match r.u8()? {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(Status::InvalidArgument),
        }
    }
}

impl<T: Wire> Wire for Option<T> {
    fn encode(&self, w: &mut WireWriter) {
        match self {
            None => w.u8(0),
            Some(v) => {
                w.u8(1);
                v.encode(w);
            }
        }
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        match r.u8()? {
            0 => Ok(None),
            1 => Ok(Some(T::decode(r)?)),
            _ => Err(Status::InvalidArgument),
        }
    }
}

impl<T: Wire + Copy + Default, const N: usize> Wire for [T; N] {
    fn encode(&self, w: &mut WireWriter) {
        for item in self {
            item.encode(w);
        }
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let mut out = [T::default(); N];
        for item in &mut out {
            *item = T::decode(r)?;
        }
        Ok(out)
    }
}

/// A `u8` length and the bytes; a decoder re-validates through the owning identifier.
impl<const N: usize> Wire for FixedString<N> {
    fn encode(&self, w: &mut WireWriter) {
        w.u8(self.len() as u8);
        w.bytes(self.as_bytes());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let n = r.u8()? as usize;
        let bytes = r.bytes(n)?;
        if core::str::from_utf8(bytes).is_err() {
            return Err(Status::InvalidArgument);
        }
        FixedString::from_bytes(bytes)
    }
}

/// A `u32` length and the items; the decoded vector's capacity is its length.
impl<T: Wire> Wire for FixedVec<T> {
    fn encode(&self, w: &mut WireWriter) {
        w.u32(self.len() as u32);
        for item in self {
            item.encode(w);
        }
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let n = r.u32()? as usize;
        if n > r.remaining() {
            return Err(Status::Truncated); // every item takes at least one byte
        }
        let mut out = FixedVec::with_capacity(n);
        for _ in 0..n {
            out.push(T::decode(r)?)?;
        }
        Ok(out)
    }
}

impl Wire for UnixNanos {
    fn encode(&self, w: &mut WireWriter) {
        w.u64(self.value());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Ok(UnixNanos::new(r.u64()?))
    }
}
impl Wire for DurationNanos {
    fn encode(&self, w: &mut WireWriter) {
        w.u64(self.value());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Ok(DurationNanos::new(r.u64()?))
    }
}
wire_struct!(EventKey { ts, source_id, seq });
wire_struct!(TimerKey { owner, id });

// ---- model value types --------------------------------------------------------------------------

use crate::currency::Currency;
use crate::fixed_point::{Price, Quantity, PRICE_RAW_UNDEF, QUANTITY_RAW_UNDEF};
use crate::identifiers::{
    AccountId, ClientId, ClientOrderId, ComponentId, ExecAlgorithmId, InstrumentId, OrderListId,
    PositionId, StrategyId, Symbol, TradeId, TraderId, Venue, VenueOrderId,
};
use crate::money::Money;
use crate::uuid::Uuid4;

/// `raw: i64, precision: u8`; the sentinel `UNDEF` is allowed and decodes back to it.
impl Wire for Price {
    fn encode(&self, w: &mut WireWriter) {
        w.u64(self.raw() as u64);
        w.u8(self.precision());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let raw = r.u64()? as i64;
        let precision = r.u8()?;
        if raw == PRICE_RAW_UNDEF {
            if precision != 0 {
                return Err(Status::InvalidArgument);
            }
            return Ok(Price::undef());
        }
        Price::from_raw(raw, precision)
    }
}
impl Wire for Quantity {
    fn encode(&self, w: &mut WireWriter) {
        w.u64(self.raw());
        w.u8(self.precision());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let raw = r.u64()?;
        let precision = r.u8()?;
        if raw == QUANTITY_RAW_UNDEF {
            if precision != 0 {
                return Err(Status::InvalidArgument);
            }
            return Ok(Quantity::undef());
        }
        Quantity::from_raw(raw, precision)
    }
}
/// The whole currency, so a log is self-describing for currencies registered at run time.
impl Wire for Currency {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.code);
        w.u8(self.precision);
        w.u16(self.iso4217);
        w.put(&self.name);
        w.put(&self.currency_type);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let code: FixedString<8> = r.get()?;
        let precision = r.u8()?;
        let iso4217 = r.u16()?;
        let name: FixedString<32> = r.get()?;
        let currency_type = r.get()?;
        Currency::new(code.as_str(), precision, iso4217, name.as_str(), currency_type)
    }
}
impl Wire for Money {
    fn encode(&self, w: &mut WireWriter) {
        w.u64(self.raw() as u64);
        w.put(&self.currency());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let raw = r.u64()? as i64;
        let currency = r.get()?;
        Money::from_raw_exact(raw, currency)
    }
}
impl Wire for Uuid4 {
    fn encode(&self, w: &mut WireWriter) {
        w.bytes(self.as_bytes());
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        let b = r.bytes(16)?;
        let mut out = [0u8; 16];
        out.copy_from_slice(b);
        Uuid4::from_bytes(out)
    }
}

macro_rules! wire_identifier {
    ($($t:ident),+) => {
        $(impl Wire for $t {
            fn encode(&self, w: &mut WireWriter) {
                w.put(self.inner());
            }
            fn decode(r: &mut WireReader<'_>) -> Result<Self> {
                $t::from_fixed(r.get()?)
            }
        })+
    };
}
wire_identifier!(
    Symbol,
    Venue,
    TraderId,
    StrategyId,
    AccountId,
    ClientOrderId,
    ClientId,
    ComponentId,
    ExecAlgorithmId,
    OrderListId,
    VenueOrderId,
    PositionId,
    TradeId
);
wire_struct!(InstrumentId { symbol, venue });
