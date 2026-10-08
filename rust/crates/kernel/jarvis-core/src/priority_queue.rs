//! Fixed-capacity min-heap ordered by [`EventKey`].

use crate::event_key::EventKey;
use crate::fixed_vec::FixedVec;
use crate::status::Result;

/// A queued element: its key and payload.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Entry<T> {
    pub key: EventKey,
    pub payload: T,
}

/// Fixed-capacity min-heap ordered by [`EventKey`]. Keys are unique by construction
/// (docs/architecture.md section 5.2), so the pop order is fully determined by the keys and never
/// by heap layout.
#[derive(Clone, Debug)]
pub struct PriorityQueue<T> {
    heap: FixedVec<Entry<T>>,
}

impl<T> PriorityQueue<T> {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self { heap: FixedVec::with_capacity(capacity) }
    }

    pub fn push(&mut self, key: EventKey, payload: T) -> Result<()> {
        self.heap.push(Entry { key, payload })?;
        self.sift_up(self.heap.len() - 1);
        Ok(())
    }

    /// The smallest entry, if any.
    #[must_use]
    pub fn peek(&self) -> Option<&Entry<T>> {
        self.heap.first()
    }

    pub fn pop(&mut self) -> Option<Entry<T>> {
        let last = self.heap.pop()?;
        if self.heap.is_empty() {
            return Some(last);
        }
        let top = core::mem::replace(&mut self.heap[0], last);
        self.sift_down(0);
        Some(top)
    }

    /// The entry with the smallest key among those for which `matches` is true, without changing
    /// the heap. It descends below an entry only when that entry does not match (a match's
    /// descendants have larger keys), so it reads about as many entries as there are non-matching
    /// ones above the answer.
    #[must_use]
    pub fn min_where(&self, mut matches: impl FnMut(&Entry<T>) -> bool) -> Option<&Entry<T>> {
        // Depth-first with an explicit stack: one pending right child per level, and a heap of at
        // most 2^64 entries has 64 levels, so 128 slots never overflow and nothing allocates.
        let mut pending = [0usize; 128];
        let mut depth = 0;
        let mut best: Option<&Entry<T>> = None;
        if !self.heap.is_empty() {
            pending[0] = 0;
            depth = 1;
        }
        while depth > 0 {
            depth -= 1;
            let i = pending[depth];
            let entry = &self.heap[i];
            if best.is_some_and(|b| entry.key >= b.key) {
                continue; // nothing below it is smaller either
            }
            if matches(entry) {
                best = Some(entry);
                continue;
            }
            for child in [2 * i + 2, 2 * i + 1] {
                if child < self.heap.len() {
                    pending[depth] = child;
                    depth += 1;
                }
            }
        }
        best
    }

    /// Keeps only the entries for which `keep` is true and restores the heap, without
    /// allocating. Used to purge cancelled timers.
    pub fn retain(&mut self, keep: impl FnMut(&Entry<T>) -> bool) {
        self.heap.retain(keep);
        for i in (0..self.heap.len() / 2).rev() {
            self.sift_down(i);
        }
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.heap.len()
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.heap.is_empty()
    }
    #[must_use]
    pub fn is_full(&self) -> bool {
        self.heap.is_full()
    }
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.heap.capacity()
    }
    pub fn clear(&mut self) {
        self.heap.clear();
    }
    /// The entries in heap order (a valid heap, not sorted); the snapshot encoding stores them.
    #[must_use]
    pub fn as_heap(&self) -> &FixedVec<Entry<T>> {
        &self.heap
    }
    pub(crate) fn heap_mut(&mut self) -> &mut FixedVec<Entry<T>> {
        &mut self.heap
    }

    fn sift_up(&mut self, mut i: usize) {
        while i > 0 {
            let parent = (i - 1) / 2;
            if self.heap[i].key >= self.heap[parent].key {
                break;
            }
            self.heap.swap(i, parent);
            i = parent;
        }
    }

    fn sift_down(&mut self, mut i: usize) {
        let n = self.heap.len();
        loop {
            let (left, right) = (2 * i + 1, 2 * i + 2);
            let mut smallest = i;
            if left < n && self.heap[left].key < self.heap[smallest].key {
                smallest = left;
            }
            if right < n && self.heap[right].key < self.heap[smallest].key {
                smallest = right;
            }
            if smallest == i {
                return;
            }
            self.heap.swap(i, smallest);
            i = smallest;
        }
    }
}
