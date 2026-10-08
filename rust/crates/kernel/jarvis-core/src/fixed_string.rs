//! Inline string with a fixed capacity.

use core::cmp::Ordering;

use crate::status::{Result, Status};

/// Inline string with a fixed capacity of at most 255 bytes. `Copy`, so it can live in events,
/// arenas and the event log without heap allocation. Comparison is by content.
#[derive(Clone, Copy)]
pub struct FixedString<const N: usize> {
    data: [u8; N],
    len: u8,
}

impl<const N: usize> FixedString<N> {
    const CAPACITY_FITS_A_BYTE: () =
        assert!(N > 0 && N <= 255, "FixedString capacity must fit in one byte");

    #[must_use]
    pub const fn new() -> Self {
        let () = Self::CAPACITY_FITS_A_BYTE;
        Self { data: [0; N], len: 0 }
    }

    /// `OutOfRange` when `text` is longer than the capacity.
    pub fn from_text(text: &str) -> Result<Self> {
        Self::from_bytes(text.as_bytes())
    }

    /// `OutOfRange` when `bytes` is longer than the capacity. The bytes are stored as given; the
    /// model layer decides what characters its strings admit.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self> {
        let () = Self::CAPACITY_FITS_A_BYTE;
        if bytes.len() > N {
            return Err(Status::OutOfRange);
        }
        let mut value = Self::new();
        value.data[..bytes.len()].copy_from_slice(bytes);
        value.len = bytes.len() as u8;
        Ok(value)
    }

    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.len as usize]
    }

    /// The text. Every constructor took valid UTF-8 or raw bytes the caller vouched for; invalid
    /// bytes read as replacement characters rather than failing.
    #[must_use]
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(self.as_bytes()).unwrap_or("\u{FFFD}")
    }

    #[must_use]
    pub const fn len(&self) -> usize {
        self.len as usize
    }
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }
    #[must_use]
    pub const fn capacity() -> usize {
        N
    }
}

impl<const N: usize> core::str::FromStr for FixedString<N> {
    type Err = Status;
    fn from_str(text: &str) -> Result<Self> {
        Self::from_text(text)
    }
}

impl<const N: usize> Default for FixedString<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> PartialEq for FixedString<N> {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}
impl<const N: usize> Eq for FixedString<N> {}

impl<const N: usize> PartialOrd for FixedString<N> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl<const N: usize> Ord for FixedString<N> {
    fn cmp(&self, other: &Self) -> Ordering {
        self.as_bytes().cmp(other.as_bytes())
    }
}

impl<const N: usize> core::hash::Hash for FixedString<N> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.as_bytes().hash(state);
    }
}

impl<const N: usize> core::fmt::Debug for FixedString<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "FixedString<{N}>({:?})", self.as_str())
    }
}

impl<const N: usize> core::fmt::Display for FixedString<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.as_str())
    }
}
