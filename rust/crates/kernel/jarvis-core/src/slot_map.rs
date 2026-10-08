//! A fixed-capacity slot map: values addressed by generation-tagged handles.
//!
//! This is what the architecture calls the "arena" (docs/architecture.md section 6.8, D24). It is
//! not an allocator: it owns values of one type by value, and hands out [`Handle`]s, which are
//! indices with a generation, never pointers. Handles go into snapshots and logs and mean the
//! same thing in a replay; a handle to an erased value never aliases the value that reuses the
//! slot. Slot reuse is LIFO, so the same sequence of operations always yields the same handles.

use alloc::vec::Vec;
use core::marker::PhantomData;

use crate::status::{Result, Status};

/// Index into a [`SlotMap`] plus the generation of the slot when the handle was issued.
///
/// `Tag` distinguishes handle types (an order handle is not a timer handle). It carries no data
/// and imposes no bounds; the manual trait impls below exist so that `Handle<Tag>` is `Copy`,
/// `Ord` and `Hash` whatever `Tag` is.
pub struct Handle<Tag> {
    pub index: u32,
    pub generation: u32,
    tag: PhantomData<fn() -> Tag>,
}

impl<Tag> Handle<Tag> {
    #[must_use]
    pub const fn new(index: u32, generation: u32) -> Self {
        Self { index, generation, tag: PhantomData }
    }
}

impl<Tag> Clone for Handle<Tag> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<Tag> Copy for Handle<Tag> {}
impl<Tag> Default for Handle<Tag> {
    fn default() -> Self {
        Self::new(0, 0)
    }
}
impl<Tag> PartialEq for Handle<Tag> {
    fn eq(&self, other: &Self) -> bool {
        (self.index, self.generation) == (other.index, other.generation)
    }
}
impl<Tag> Eq for Handle<Tag> {}
impl<Tag> PartialOrd for Handle<Tag> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl<Tag> Ord for Handle<Tag> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        (self.index, self.generation).cmp(&(other.index, other.generation))
    }
}
impl<Tag> core::hash::Hash for Handle<Tag> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        (self.index, self.generation).hash(state);
    }
}
impl<Tag> core::fmt::Debug for Handle<Tag> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Handle({}@{})", self.index, self.generation)
    }
}

#[derive(Clone, Debug)]
struct Slot<T> {
    value: Option<T>,
    generation: u32,
}

/// Fixed-capacity slot map. Capacity is fixed at construction; `insert` returns
/// [`Status::CapacityExceeded`] when full and nothing allocates afterwards.
pub struct SlotMap<T, Tag = T> {
    slots: Vec<Slot<T>>,
    free: Vec<u32>,
    tag: PhantomData<fn() -> Tag>,
}

impl<T, Tag> SlotMap<T, Tag> {
    #[must_use]
    pub fn with_capacity(capacity: u32) -> Self {
        let mut slots = Vec::with_capacity(capacity as usize);
        slots.resize_with(capacity as usize, || Slot { value: None, generation: 0 });
        Self { slots, free: (0..capacity).rev().collect(), tag: PhantomData }
    }

    pub fn insert(&mut self, value: T) -> Result<Handle<Tag>> {
        let index = self.free.pop().ok_or(Status::CapacityExceeded)?;
        let slot = &mut self.slots[index as usize];
        slot.value = Some(value);
        Ok(Handle::new(index, slot.generation))
    }

    #[must_use]
    pub fn get(&self, handle: Handle<Tag>) -> Option<&T> {
        self.slot(handle).and_then(|slot| slot.value.as_ref())
    }

    #[must_use]
    pub fn get_mut(&mut self, handle: Handle<Tag>) -> Option<&mut T> {
        let slot = self.slots.get_mut(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        slot.value.as_mut()
    }

    /// Removes and returns the value; `NotFound` for a stale or unknown handle. The slot's
    /// generation advances, so the handle is dead from now on.
    pub fn remove(&mut self, handle: Handle<Tag>) -> Result<T> {
        let slot = self.slots.get_mut(handle.index as usize).ok_or(Status::NotFound)?;
        if slot.generation != handle.generation {
            return Err(Status::NotFound);
        }
        let value = slot.value.take().ok_or(Status::NotFound)?;
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(handle.index);
        Ok(value)
    }

    #[must_use]
    pub fn contains(&self, handle: Handle<Tag>) -> bool {
        self.get(handle).is_some()
    }

    /// Live entries in slot order, which is deterministic.
    pub fn iter(&self) -> impl Iterator<Item = (Handle<Tag>, &T)> {
        self.slots.iter().enumerate().filter_map(|(i, slot)| {
            slot.value.as_ref().map(|v| (Handle::new(i as u32, slot.generation), v))
        })
    }

    pub fn iter_mut(&mut self) -> impl Iterator<Item = (Handle<Tag>, &mut T)> {
        self.slots.iter_mut().enumerate().filter_map(|(i, slot)| {
            slot.value.as_mut().map(|v| (Handle::new(i as u32, slot.generation), v))
        })
    }

    pub fn values(&self) -> impl Iterator<Item = &T> {
        self.slots.iter().filter_map(|slot| slot.value.as_ref())
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.slots.len() - self.free.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    #[must_use]
    pub fn is_full(&self) -> bool {
        self.free.is_empty()
    }
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.slots.len()
    }

    fn slot(&self, handle: Handle<Tag>) -> Option<&Slot<T>> {
        self.slots.get(handle.index as usize).filter(|slot| slot.generation == handle.generation)
    }

    // ---- snapshot encoding (state.rs) ---------------------------------------------------------

    /// Every slot's generation and value, then the free list in its order (it decides the next
    /// handles).
    pub(crate) fn write_state(&self, w: &mut crate::state::StateWriter<'_>)
    where
        T: crate::state::State + Default,
    {
        use crate::state::State as _;
        w.u32(self.slots.len() as u32);
        for slot in &self.slots {
            w.u32(slot.generation);
            slot.value.write(w);
        }
        w.u32(self.free.len() as u32);
        for &index in &self.free {
            w.u32(index);
        }
    }

    /// Restores into a map of the same capacity; anything else fails the reader.
    pub(crate) fn read_state(&mut self, r: &mut crate::state::StateReader<'_>)
    where
        T: crate::state::State + Default,
    {
        use crate::state::State as _;
        if r.u32() as usize != self.slots.len() {
            r.fail(Status::CapacityExceeded);
            return;
        }
        for slot in &mut self.slots {
            slot.generation = r.u32();
            slot.value.read(r);
        }
        let free = crate::state::read_length(r, self.slots.len());
        self.free.clear();
        for _ in 0..free {
            let index = r.u32();
            let occupied = self.slots.get(index as usize).is_none_or(|s| s.value.is_some());
            if occupied || self.free.contains(&index) {
                r.fail(Status::InvalidState);
                return;
            }
            self.free.push(index);
        }
        if r.is_ok() && self.free.len() != self.slots.iter().filter(|s| s.value.is_none()).count() {
            r.fail(Status::InvalidState);
        }
    }
}

impl<T: Clone, Tag> Clone for SlotMap<T, Tag> {
    fn clone(&self) -> Self {
        Self { slots: self.slots.clone(), free: self.free.clone(), tag: PhantomData }
    }
}

impl<T: core::fmt::Debug, Tag> core::fmt::Debug for SlotMap<T, Tag> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_map().entries(self.iter()).finish()
    }
}

impl<T, Tag> core::ops::Index<Handle<Tag>> for SlotMap<T, Tag> {
    type Output = T;
    /// Panics on a dead handle: indexing asserts the caller knows the value is live; use `get`
    /// when it may not be.
    fn index(&self, handle: Handle<Tag>) -> &T {
        self.get(handle).expect("SlotMap: dead handle")
    }
}
