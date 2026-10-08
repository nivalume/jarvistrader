//! Fixed-capacity interning: a key seen for the first time takes the next slot, so slots are
//! deterministic given the event order (docs/architecture.md section 6.2). Lookups by key are
//! linear and happen off the hot path (when an event arrives with a string id); the kernel
//! addresses everything else by slot.

use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{FixedVec, Result, Status};
use model::bar::BarType;
use model::instruments::Instrument;
use model::wire::{Wire, WireReader, WireWriter};
use model::InstrumentId;

/// Keys interned into dense `u32` slots.
#[derive(Clone, Debug)]
pub struct InternTable<K> {
    keys: FixedVec<K>,
}

impl<K: Copy + Eq> InternTable<K> {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self { keys: FixedVec::with_capacity(capacity) }
    }

    /// The slot of `key`, assigning the next one when it is new; `CapacityExceeded` when full.
    pub fn intern(&mut self, key: K) -> Result<u32> {
        if let Some(slot) = self.slot_of(&key) {
            return Ok(slot);
        }
        self.keys.push(key)?;
        Ok(self.keys.len() as u32 - 1)
    }

    #[must_use]
    pub fn slot_of(&self, key: &K) -> Option<u32> {
        self.keys.iter().position(|k| k == key).map(|i| i as u32)
    }

    #[must_use]
    pub fn key_of(&self, slot: u32) -> Option<&K> {
        self.keys.get(slot as usize)
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.keys.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.keys.capacity()
    }
    pub fn keys(&self) -> impl Iterator<Item = (u32, &K)> {
        self.keys.iter().enumerate().map(|(i, k)| (i as u32, k))
    }
}

/// Keys are written in their wire encoding (canonical and self-delimiting); the capacity comes
/// from the configuration and must match.
impl<K: Wire + Copy + Eq> State for InternTable<K> {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.keys.capacity() as u32);
        w.u32(self.keys.len() as u32);
        for key in &self.keys {
            let mut out = WireWriter::new();
            key.encode(&mut out);
            w.raw(out.as_slice());
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        if r.u32() as usize != self.keys.capacity() {
            r.fail(Status::CapacityExceeded);
            return;
        }
        let n = kernel_core::state::read_length(r, self.keys.capacity());
        self.keys.clear();
        for _ in 0..n {
            let mut wire = WireReader::new(r.rest());
            match K::decode(&mut wire) {
                Ok(key) => {
                    r.skip(wire.position());
                    let _ = self.keys.push(key);
                }
                Err(e) => {
                    r.fail(e);
                    return;
                }
            }
        }
    }
}

/// The slot of an instrument in the node's tables.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct InstrumentSlot(pub u32);

impl InstrumentSlot {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}
impl State for InstrumentSlot {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.0);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.0 = r.u32();
    }
}

/// The slot of a bar type.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BarKey(pub u32);

impl BarKey {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}
impl State for BarKey {
    fn write(&self, w: &mut StateWriter<'_>) {
        w.u32(self.0);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.0 = r.u32();
    }
}

/// Instrument ids and, once an `Instrument` event arrives, their definitions.
#[derive(Clone, Debug)]
pub struct InstrumentTable {
    ids: InternTable<InstrumentId>,
    definitions: FixedVec<Option<Instrument>>,
}

impl InstrumentTable {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        let mut definitions = FixedVec::with_capacity(capacity);
        for _ in 0..capacity {
            let _ = definitions.push(None);
        }
        Self { ids: InternTable::with_capacity(capacity), definitions }
    }

    pub fn intern(&mut self, id: InstrumentId) -> Result<InstrumentSlot> {
        self.ids.intern(id).map(InstrumentSlot)
    }
    #[must_use]
    pub fn slot_of(&self, id: &InstrumentId) -> Option<InstrumentSlot> {
        self.ids.slot_of(id).map(InstrumentSlot)
    }
    #[must_use]
    pub fn id_of(&self, slot: InstrumentSlot) -> Option<&InstrumentId> {
        self.ids.key_of(slot.0)
    }

    /// Records the definition (an `Instrument` input); the id is interned if new.
    pub fn define(&mut self, instrument: Instrument) -> Result<InstrumentSlot> {
        let slot = self.intern(instrument.id)?;
        self.definitions[slot.index()] = Some(instrument);
        Ok(slot)
    }
    /// The definition, if one has arrived.
    #[must_use]
    pub fn instrument(&self, slot: InstrumentSlot) -> Option<&Instrument> {
        self.definitions.get(slot.index()).and_then(Option::as_ref)
    }
    /// The definition, or `NotFound`: the caller needs tick sizes or limits.
    pub fn require(&self, slot: InstrumentSlot) -> Result<&Instrument> {
        self.instrument(slot).ok_or(Status::NotFound)
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.ids.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.ids.is_empty()
    }
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.ids.capacity()
    }
}

/// Instruments are large and their snapshot is the wire encoding, not a field list.
impl State for InstrumentTable {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.ids.write(w);
        for definition in &self.definitions {
            match definition {
                None => w.u8(0),
                Some(i) => {
                    w.u8(1);
                    let mut out = WireWriter::new();
                    i.encode(&mut out);
                    w.raw(out.as_slice());
                }
            }
        }
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.ids.read(r);
        for definition in &mut self.definitions {
            *definition = match r.u8() {
                0 => None,
                1 => {
                    let mut wire = WireReader::new(r.rest());
                    match Instrument::decode(&mut wire) {
                        Ok(i) => {
                            r.skip(wire.position());
                            Some(i)
                        }
                        Err(e) => {
                            r.fail(e);
                            None
                        }
                    }
                }
                _ => {
                    r.fail(Status::InvalidArgument);
                    None
                }
            };
        }
    }
}

/// Bar types, interned to keys. INTERNAL bar types the strategies declare and EXTERNAL ones that
/// arrive as data share the table.
pub type BarTable = InternTable<BarType>;
