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

    pub fn push(&mut self, value: T) -> Result<()> {
        if self.items.len() >= self.capacity {
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

    /// Replaces the capacity and contents at once; the snapshot reader uses it for a vector that
    /// was built empty (capacity 0) and takes its size from the saved state.
    pub(crate) fn reset_with_capacity(&mut self, capacity: usize) {
        self.items = Vec::with_capacity(capacity);
        self.capacity = capacity;
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

impl<T: Default + Clone> FixedVec<T> {
    /// Sets the length to `n` default elements; `CapacityExceeded` past the capacity.
    pub fn resize_default(&mut self, n: usize) -> Result<()> {
        if n > self.capacity {
            return Err(Status::CapacityExceeded);
        }
        self.items.clear();
        self.items.resize(n, T::default());
        Ok(())
    }
}
