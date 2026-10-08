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
    pub(crate) heap: FixedVec<Entry<T>>,
}

impl<T: Copy> PriorityQueue<T> {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self { heap: FixedVec::with_capacity(capacity) }
    }

    pub fn push(&mut self, key: EventKey, payload: T) -> Result<()> {
        self.heap.push(Entry { key, payload })?;
        let last = self.heap.len() - 1;
        self.sift_up(last);
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

    /// The entry with the smallest key among those for which `matches(entry)` is true, without
    /// changing the heap: it looks below an entry only when that entry does not match (a match's
    /// descendants have larger keys), so it reads about as many entries as there are non-matching
    /// ones above the answer.
    #[must_use]
    pub fn min_where(&self, mut matches: impl FnMut(&Entry<T>) -> bool) -> Option<&Entry<T>> {
        let mut best: Option<&Entry<T>> = None;
        // Depth first; a pending right child per level at most, and a heap is at most 64 levels.
        let mut pending = [0usize; 128];
        let mut depth = 0;
        if !self.heap.is_empty() {
            pending[depth] = 0;
            depth += 1;
        }
        while depth > 0 {
            depth -= 1;
            let i = pending[depth];
            let e = &self.heap[i];
            if let Some(b) = best {
                if e.key >= b.key {
                    continue; // nothing below it is smaller either
                }
            }
            if matches(e) {
                best = Some(e);
                continue;
            }
            let left = 2 * i + 1;
            if left + 1 < self.heap.len() {
                pending[depth] = left + 1;
                depth += 1;
            }
            if left < self.heap.len() {
                pending[depth] = left;
                depth += 1;
            }
        }
        best
    }

    /// Keeps only entries for which `keep(entry)` is true, then restores the heap. Used to purge
    /// cancelled entries without allocating.
    pub fn retain(&mut self, mut keep: impl FnMut(&Entry<T>) -> bool) {
        let mut write = 0;
        for read in 0..self.heap.len() {
            if keep(&self.heap[read]) {
                self.heap[write] = self.heap[read];
                write += 1;
            }
        }
        self.heap.truncate(write);
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
            let left = 2 * i + 1;
            let right = left + 1;
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
