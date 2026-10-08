//! Fixed-capacity slab addressed by generation-tagged handles.

use alloc::vec::Vec;
use core::marker::PhantomData;

use crate::status::{Result, Status};

/// Index into an [`Arena`] plus the generation of the slot at the time the handle was issued. A
/// handle to an erased element never aliases the element that later reuses the slot (no ABA).
///
/// `Tag` only distinguishes handle types (an order handle is not a timer handle); it carries no
/// data and places no bounds on the handle.
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
        self.index == other.index && self.generation == other.generation
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
        self.index.hash(state);
        self.generation.hash(state);
    }
}
impl<Tag> core::fmt::Debug for Handle<Tag> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Handle({}@{})", self.index, self.generation)
    }
}

#[derive(Clone, Debug, Default)]
pub(crate) struct Slot<T> {
    pub(crate) value: T,
    pub(crate) generation: u32,
    pub(crate) occupied: bool,
}

/// Fixed-capacity slab of `T` addressed by generation-tagged handles. Capacity is fixed at
/// construction; `insert` returns [`Status::CapacityExceeded`] when full. Slot reuse is LIFO, so
/// the same sequence of operations always yields the same handles.
pub struct Arena<T, Tag = T> {
    pub(crate) slots: Vec<Slot<T>>,
    pub(crate) free: Vec<u32>,
    pub(crate) len: usize,
    tag: PhantomData<fn() -> Tag>,
}

impl<T: Clone, Tag> Clone for Arena<T, Tag> {
    fn clone(&self) -> Self {
        Self { slots: self.slots.clone(), free: self.free.clone(), len: self.len, tag: PhantomData }
    }
}

impl<T: core::fmt::Debug, Tag> core::fmt::Debug for Arena<T, Tag> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Arena")
            .field("slots", &self.slots)
            .field("free", &self.free)
            .field("len", &self.len)
            .finish()
    }
}

impl<T: Default, Tag> Arena<T, Tag> {
    #[must_use]
    pub fn with_capacity(capacity: u32) -> Self {
        let mut slots = Vec::with_capacity(capacity as usize);
        slots.resize_with(capacity as usize, Slot::default);
        let mut free = Vec::with_capacity(capacity as usize);
        for i in (0..capacity).rev() {
            free.push(i);
        }
        Self { slots, free, len: 0, tag: PhantomData }
    }
}

impl<T, Tag> Arena<T, Tag> {
    pub fn insert(&mut self, value: T) -> Result<Handle<Tag>> {
        let Some(index) = self.free.pop() else { return Err(Status::CapacityExceeded) };
        let slot = &mut self.slots[index as usize];
        slot.value = value;
        slot.occupied = true;
        self.len += 1;
        Ok(Handle::new(index, slot.generation))
    }

    #[must_use]
    pub fn get(&self, handle: Handle<Tag>) -> Option<&T> {
        if self.is_valid(handle) {
            Some(&self.slots[handle.index as usize].value)
        } else {
            None
        }
    }

    #[must_use]
    pub fn get_mut(&mut self, handle: Handle<Tag>) -> Option<&mut T> {
        if self.is_valid(handle) {
            Some(&mut self.slots[handle.index as usize].value)
        } else {
            None
        }
    }

    /// Frees the slot; the value stays in place until the slot is reused (nothing is dropped
    /// early, nothing is reset: `T: Default` is only needed to build the arena).
    pub fn erase(&mut self, handle: Handle<Tag>) -> Result<()> {
        if !self.is_valid(handle) {
            return Err(Status::NotFound);
        }
        let slot = &mut self.slots[handle.index as usize];
        slot.occupied = false;
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(handle.index);
        self.len -= 1;
        Ok(())
    }

    #[must_use]
    pub fn is_valid(&self, handle: Handle<Tag>) -> bool {
        match self.slots.get(handle.index as usize) {
            Some(slot) => slot.occupied && slot.generation == handle.generation,
            None => false,
        }
    }

    /// Visits live elements in slot order, which is deterministic.
    pub fn for_each(&self, mut visit: impl FnMut(Handle<Tag>, &T)) {
        for (i, slot) in self.slots.iter().enumerate() {
            if slot.occupied {
                visit(Handle::new(i as u32, slot.generation), &slot.value);
            }
        }
    }

    /// Live elements in slot order.
    pub fn iter(&self) -> impl Iterator<Item = (Handle<Tag>, &T)> {
        self.slots
            .iter()
            .enumerate()
            .filter(|(_, slot)| slot.occupied)
            .map(|(i, slot)| (Handle::new(i as u32, slot.generation), &slot.value))
    }

    #[must_use]
    pub const fn len(&self) -> usize {
        self.len
    }
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.slots.len()
    }
}
