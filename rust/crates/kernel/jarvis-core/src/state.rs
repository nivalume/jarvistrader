//! The explicit encoding of kernel state for `EngineState` snapshots (docs/architecture.md
//! section 16.3). Like the event log it never writes struct padding, pointers or container
//! capacity the configuration already fixes: every value is written field by field,
//! little-endian, at a fixed width. The bytes are those of `jarvis/core/state.hpp` in the C++
//! tree, so a snapshot written by one tree loads in the other.
//!
//! A stateful type implements [`State`] once for both directions, usually through
//! [`state_fields!`]:
//!
//! ```
//! use jarvis_core::state_fields;
//! #[derive(Default)]
//! struct Counter { a: u32, b: u64 }
//! state_fields!(Counter { a, b });
//! ```
//!
//! A reader restores into an object built from the same configuration, so fixed capacities must
//! match; a mismatch, a short input or an out-of-range value fails the reader (sticky), and the
//! caller discards the half-restored object.

use alloc::vec::Vec;

use crate::clock::{Timer, TimerKey, TimerQueue};
use crate::event_key::EventKey;
use crate::fixed_string::FixedString;
use crate::fixed_vec::FixedVec;
use crate::priority_queue::{Entry, PriorityQueue};
use crate::slot_map::{Handle, SlotMap};
use crate::status::{Result, Status};
use crate::time::{DurationNanos, UnixNanos};

/// Container lengths are u32; nothing in the kernel holds more.
pub const MAX_STATE_ELEMENTS: u32 = 1 << 28;

/// Appends the encoding to a byte vector.
#[derive(Debug)]
pub struct StateWriter<'a> {
    out: &'a mut Vec<u8>,
    status: Option<Status>,
}

impl<'a> StateWriter<'a> {
    pub fn new(out: &'a mut Vec<u8>) -> Self {
        Self { out, status: None }
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
    pub fn raw(&mut self, bytes: &[u8]) {
        self.out.extend_from_slice(bytes);
    }

    /// Records the first failure; later ones are ignored.
    pub fn fail(&mut self, status: Status) {
        if self.status.is_none() {
            self.status = Some(status);
        }
    }
    #[must_use]
    pub fn is_ok(&self) -> bool {
        self.status.is_none()
    }
    pub fn status(&self) -> Result<()> {
        match self.status {
            None => Ok(()),
            Some(s) => Err(s),
        }
    }
    #[must_use]
    pub fn len(&self) -> usize {
        self.out.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.out.is_empty()
    }
}

/// Decodes from a byte slice. A failure is sticky: every later read returns zero and the status
/// stays the first error.
#[derive(Debug)]
pub struct StateReader<'a> {
    input: &'a [u8],
    pos: usize,
    status: Option<Status>,
}

impl<'a> StateReader<'a> {
    #[must_use]
    pub const fn new(input: &'a [u8]) -> Self {
        Self { input, pos: 0, status: None }
    }

    pub fn u8(&mut self) -> u8 {
        self.take(1).map_or(0, |b| b[0])
    }
    pub fn u16(&mut self) -> u16 {
        self.take(2).map_or(0, |b| u16::from_le_bytes([b[0], b[1]]))
    }
    pub fn u32(&mut self) -> u32 {
        self.take(4).map_or(0, |b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    pub fn u64(&mut self) -> u64 {
        self.take(8)
            .map_or(0, |b| u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
    }
    /// The next `n` bytes, or an empty slice (and `Truncated`) when fewer remain.
    pub fn raw(&mut self, n: usize) -> &'a [u8] {
        self.take(n).unwrap_or(&[])
    }
    /// What is left, for decoders that report how much they consumed ([`skip`]).
    ///
    /// [`skip`]: StateReader::skip
    #[must_use]
    pub fn rest(&self) -> &'a [u8] {
        &self.input[self.pos..]
    }
    pub fn skip(&mut self, n: usize) {
        let _ = self.take(n);
    }

    pub fn fail(&mut self, status: Status) {
        if self.status.is_none() {
            self.status = Some(status);
        }
    }
    pub fn check(&mut self, result: Result<()>) {
        if let Err(s) = result {
            self.fail(s);
        }
    }
    #[must_use]
    pub fn is_ok(&self) -> bool {
        self.status.is_none()
    }
    pub fn status(&self) -> Result<()> {
        match self.status {
            None => Ok(()),
            Some(s) => Err(s),
        }
    }
    #[must_use]
    pub fn remaining(&self) -> usize {
        self.input.len() - self.pos
    }

    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.status.is_some() || n > self.input.len() - self.pos {
            self.fail(Status::Truncated);
            return None;
        }
        let out = &self.input[self.pos..self.pos + n];
        self.pos += n;
        Some(out)
    }
}

/// A type with a snapshot encoding. `write` never modifies the value; `read` assigns it.
pub trait State {
    fn write(&self, w: &mut StateWriter<'_>);
    fn read(&mut self, r: &mut StateReader<'_>);
}

/// Encodes `value`.
pub fn save_state<T: State + ?Sized>(value: &T) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    save_state_into(value, &mut out)?;
    Ok(out)
}

/// Appends the encoding of `value` to `out`.
pub fn save_state_into<T: State + ?Sized>(value: &T, out: &mut Vec<u8>) -> Result<()> {
    let mut w = StateWriter::new(out);
    value.write(&mut w);
    w.status()
}

/// Restores `value` from all of `input`; bytes left over are an error.
pub fn load_state<T: State + ?Sized>(value: &mut T, input: &[u8]) -> Result<()> {
    let mut r = StateReader::new(input);
    value.read(&mut r);
    r.status()?;
    if r.remaining() != 0 {
        return Err(Status::InvalidArgument);
    }
    Ok(())
}

/// Implements [`State`] for a struct by listing its fields in encoding order.
#[macro_export]
macro_rules! state_fields {
    ($t:ty { $($f:ident),+ $(,)? }) => {
        impl $crate::state::State for $t {
            fn write(&self, w: &mut $crate::state::StateWriter<'_>) {
                $( $crate::state::State::write(&self.$f, w); )+
            }
            fn read(&mut self, r: &mut $crate::state::StateReader<'_>) {
                $( $crate::state::State::read(&mut self.$f, r); )+
            }
        }
    };
}

/// Implements [`State`] for a field-less enum with an explicit integer representation: the
/// discriminant at that width, and `InvalidArgument` for an unknown value on read.
#[macro_export]
macro_rules! state_enum {
    ($t:ty : $repr:ident { $($variant:ident),+ $(,)? }) => {
        impl $crate::state::State for $t {
            fn write(&self, w: &mut $crate::state::StateWriter<'_>) {
                $crate::state::State::write(&(*self as $repr), w);
            }
            fn read(&mut self, r: &mut $crate::state::StateReader<'_>) {
                let mut raw: $repr = 0;
                $crate::state::State::read(&mut raw, r);
                $( if raw == <$t>::$variant as $repr { *self = <$t>::$variant; return; } )+
                r.fail($crate::Status::InvalidArgument);
            }
        }
    };
}

// ---- integers and bool ------------------------------------------------------------------------

macro_rules! state_int {
    ($t:ty, $w:ident, $r:ident, $u:ty) => {
        impl State for $t {
            fn write(&self, w: &mut StateWriter<'_>) {
                w.$w(*self as $u);
            }
            fn read(&mut self, r: &mut StateReader<'_>) {
                *self = r.$r() as $t;
            }
        }
    };
}
state_int!(u8, u8, u8, u8);
state_int!(i8, u8, u8, u8);
state_int!(u16, u16, u16, u16);
state_int!(i16, u16, u16, u16);
state_int!(u32, u32, u32, u32);
state_int!(i32, u32, u32, u32);
state_int!(u64, u64, u64, u64);
state_int!(i64, u64, u64, u64);

impl State for u128 {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u64(*self as u64);
        w.u64((*self >> 64) as u64);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let lo = r.u64();
        let hi = r.u64();
        *self = (u128::from(hi) << 64) | u128::from(lo);
    }
}
impl State for i128 {
    fn write(&self, w: &mut StateWriter<'_>) {
        (*self as u128).write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let mut u: u128 = 0;
        u.read(r);
        *self = u as i128;
    }
}

impl State for bool {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u8(u8::from(*self));
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let b = r.u8();
        if b > 1 {
            r.fail(Status::InvalidArgument);
        }
        *self = b == 1;
    }
}

// ---- core and standard types ------------------------------------------------------------------

impl State for UnixNanos {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u64(self.value());
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        *self = UnixNanos::new(r.u64());
    }
}
impl State for DurationNanos {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u64(self.value());
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        *self = DurationNanos::new(r.u64());
    }
}
state_fields!(EventKey { ts, source_id, seq });
state_fields!(TimerKey { owner, id });

impl<Tag> State for Handle<Tag> {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.index);
        w.u32(self.generation);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.index = r.u32();
        self.generation = r.u32();
    }
}

impl<const N: usize> State for FixedString<N> {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u8(self.len() as u8);
        w.raw(self.as_bytes());
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let n = r.u8() as usize;
        let bytes = r.raw(n);
        if !r.is_ok() {
            return;
        }
        match FixedString::from_bytes(bytes) {
            Ok(s) => *self = s,
            Err(e) => r.fail(e),
        }
    }
}

impl<T: State + Default> State for Option<T> {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.is_some().write(w);
        if let Some(v) = self {
            v.write(w);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let mut has = false;
        has.read(r);
        if !has {
            *self = None;
            return;
        }
        if self.is_none() {
            *self = Some(T::default());
        }
        if let Some(v) = self {
            v.read(r);
        }
    }
}

impl<T: State, const N: usize> State for [T; N] {
    fn write(&self, w: &mut StateWriter<'_>) {
        for item in self {
            item.write(w);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        for item in self {
            item.read(r);
        }
    }
}

impl<A: State, B: State> State for (A, B) {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.0.write(w);
        self.1.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.0.read(r);
        self.1.read(r);
    }
}

/// A length written before the elements; the reader checks it against `capacity` and fails with
/// `CapacityExceeded` past it, returning 0.
pub fn write_length(w: &mut StateWriter<'_>, n: usize) {
    w.u32(n as u32);
}
pub fn read_length(r: &mut StateReader<'_>, capacity: usize) -> usize {
    let n = r.u32();
    if n as usize > capacity || n > MAX_STATE_ELEMENTS {
        r.fail(Status::CapacityExceeded);
        return 0;
    }
    n as usize
}

/// Snapshot encoding of a fixed vector: the capacity, the length, the elements. A vector built
/// empty (capacity 0) takes the saved capacity: those are sized on first use, from data.
impl<T: State + Default> State for FixedVec<T> {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.capacity() as u32);
        w.u32(self.len() as u32);
        for item in self {
            item.write(w);
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        let capacity = r.u32() as usize;
        if capacity != self.capacity() {
            if self.capacity() != 0 || capacity > MAX_STATE_ELEMENTS as usize {
                r.fail(Status::CapacityExceeded);
                return;
            }
            *self = FixedVec::with_capacity(capacity);
        }
        let n = read_length(r, self.capacity());
        self.clear();
        for _ in 0..n {
            let mut item = T::default();
            item.read(r);
            if self.push(item).is_err() {
                r.fail(Status::CapacityExceeded);
                return;
            }
        }
    }
}

/// A vector of u64 that is mostly zeros (the levels of an order book's window), as runs: the
/// capacity and length, then (zeros u32, values u32, the values) until the length is covered.
/// Reading restores the same length and capacity check as the plain encoding.
pub fn write_sparse(w: &mut StateWriter<'_>, v: &FixedVec<u64>) {
    let n = v.len();
    w.u32(v.capacity() as u32);
    w.u32(n as u32);
    let mut i = 0;
    while i < n {
        let mut zeros = 0;
        while i + zeros < n && v[i + zeros] == 0 {
            zeros += 1;
        }
        let mut values = 0;
        while i + zeros + values < n && v[i + zeros + values] != 0 {
            values += 1;
        }
        w.u32(zeros as u32);
        w.u32(values as u32);
        for k in 0..values {
            w.u64(v[i + zeros + k]);
        }
        i += zeros + values;
    }
}
pub fn read_sparse(r: &mut StateReader<'_>, v: &mut FixedVec<u64>) {
    let capacity = r.u32() as usize;
    let n = r.u32() as usize;
    if !r.is_ok() || capacity != v.capacity() || n != v.len() {
        r.fail(Status::CapacityExceeded);
        return;
    }
    let mut i = 0;
    while i < n && r.is_ok() {
        let zeros = r.u32() as usize;
        let values = r.u32() as usize;
        if zeros + values > n - i || zeros + values == 0 {
            r.fail(Status::InvalidArgument);
            return;
        }
        for _ in 0..zeros {
            v[i] = 0;
            i += 1;
        }
        for _ in 0..values {
            v[i] = r.u64();
            i += 1;
        }
    }
}

/// Snapshot encoding of a slot map: see `SlotMap::write_state`.
impl<T: State + Default, Tag> State for SlotMap<T, Tag> {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.write_state(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.read_state(r);
    }
}

impl<T: State + Default> State for Entry<T> {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.key.write(w);
        self.payload.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.key.read(r);
        self.payload.read(r);
    }
}

/// Snapshot encoding of a priority queue: the heap array as it is (a valid heap stays one).
impl<T: State + Default> State for PriorityQueue<T> {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.as_heap().write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.heap_mut().read(r);
    }
}

state_fields!(Timer { key, period, armed_seq });
// The timer queue: the timers, the heap with its stale entries, and the arming counter.
state_fields!(TimerQueue { timers, heap, seq });
