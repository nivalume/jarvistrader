//! A vector whose capacity is fixed at construction.

use alloc::vec::Vec;
use core::ops::{Deref, DerefMut};

use crate::status::{Result, Status};

/// A vector whose capacity is fixed when it is constructed (from configuration, at start-up).
/// Growth past the capacity returns [`Status::CapacityExceeded`] instead of reallocating, so no
/// operation after construction allocates.
///
/// It dereferences to a slice for reading and in-place mutation; everything that changes the
/// length goes through the checked methods here.
#[derive(Debug)]
pub struct FixedVec<T> {
    items: Vec<T>,
    capacity: usize,
}

impl<T> FixedVec<T> {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self { items: Vec::with_capacity(capacity), capacity }
    }

    /// Builds a vector whose capacity is the number of items.
    pub fn from_iter_exact(items: impl IntoIterator<Item = T>) -> Self {
        let items: Vec<T> = items.into_iter().collect();
        Self { capacity: items.len(), items }
    }

    pub fn push(&mut self, value: T) -> Result<()> {
        if self.is_full() {
            return Err(Status::CapacityExceeded);
        }
        self.items.push(value);
        Ok(())
    }

    /// Removes the element at `index` by moving the last element into its place. O(1); does not
    /// preserve order.
    pub fn swap_remove(&mut self, index: usize) -> Result<T> {
        if index >= self.items.len() {
            return Err(Status::OutOfRange);
        }
        Ok(self.items.swap_remove(index))
    }

    pub fn pop(&mut self) -> Option<T> {
        self.items.pop()
    }
    pub fn clear(&mut self) {
        self.items.clear();
    }
    /// Drops the elements past `len`; a no-op when `len` is not smaller than the length.
    pub fn truncate(&mut self, len: usize) {
        self.items.truncate(len);
    }
    /// Keeps the elements for which `keep` is true, in order, without allocating.
    pub fn retain(&mut self, keep: impl FnMut(&T) -> bool) {
        self.items.retain(keep);
    }

    #[must_use]
    pub const fn capacity(&self) -> usize {
        self.capacity
    }
    #[must_use]
    pub fn is_full(&self) -> bool {
        self.items.len() >= self.capacity
    }
    #[must_use]
    pub fn as_slice(&self) -> &[T] {
        &self.items
    }
    #[must_use]
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        &mut self.items
    }
}

impl<T: Clone> Clone for FixedVec<T> {
    /// A copy keeps the full reservation, so it never reallocates later either.
    fn clone(&self) -> Self {
        let mut items = Vec::with_capacity(self.capacity);
        items.extend_from_slice(&self.items);
        Self { items, capacity: self.capacity }
    }
}

/// Equality is by contents; two vectors with different capacities but the same elements are equal.
impl<T: PartialEq> PartialEq for FixedVec<T> {
    fn eq(&self, other: &Self) -> bool {
        self.items == other.items
    }
}
impl<T: Eq> Eq for FixedVec<T> {}

impl<T> Deref for FixedVec<T> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        &self.items
    }
}
impl<T> DerefMut for FixedVec<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        &mut self.items
    }
}

impl<'a, T> IntoIterator for &'a FixedVec<T> {
    type Item = &'a T;
    type IntoIter = core::slice::Iter<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.items.iter()
    }
}
impl<'a, T> IntoIterator for &'a mut FixedVec<T> {
    type Item = &'a mut T;
    type IntoIter = core::slice::IterMut<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.items.iter_mut()
    }
}
