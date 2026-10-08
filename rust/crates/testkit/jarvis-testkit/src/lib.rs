//! Test support for the Rust tree (the port of `testkit/` in the C++ tree).
//!
//! - [`Gen`]: the splitmix64 property-test generator, same stream as the C++ `testkit::Gen`.
//! - [`for_all`]: runs a property over seeded cases; `JARVIS_PROP_SEED`, `JARVIS_PROP_ITERS` and
//!   `JARVIS_PROP_CASE` select the base seed, the case count and a single case to replay.
//! - [`CountingAlloc`] and [`AllocationScope`]: the zero-allocation gate. A test binary installs
//!   the allocator with [`install_counting_allocator!`] and asserts that a scope saw none.
//!
//! Test-only. Kernel code uses the counter-based generators in `kernel_core::rng` and never a
//! stateful one (ADR 0001, decision 4).
//!
//! `unsafe` is denied crate-wide and allowed in exactly one place: the `GlobalAlloc` impl, whose
//! methods are `unsafe fn` by the trait's definition and only forward to the system allocator.
#![deny(unsafe_code)]

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

/// Deterministic splitmix64 stream.
#[derive(Clone, Debug)]
pub struct Gen {
    state: u64,
}

impl Gen {
    #[must_use]
    pub const fn new(seed: u64) -> Self {
        Self { state: seed }
    }

    pub fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform in `[0, bound)`. Requires `bound > 0`. Lemire's multiply-shift with rejection, so
    /// the result is unbiased.
    pub fn below(&mut self, bound: u64) -> u64 {
        let mut product = u128::from(self.next_u64()) * u128::from(bound);
        let mut low = product as u64;
        if low < bound {
            let threshold = 0u64.wrapping_sub(bound) % bound;
            while low < threshold {
                product = u128::from(self.next_u64()) * u128::from(bound);
                low = product as u64;
            }
        }
        (product >> 64) as u64
    }

    /// Uniform in the closed interval `[lo, hi]`. Requires `lo <= hi`.
    pub fn range(&mut self, lo: i64, hi: i64) -> i64 {
        let width = (hi as u64).wrapping_sub(lo as u64);
        let offset = if width == u64::MAX { self.next_u64() } else { self.below(width + 1) };
        (lo as u64).wrapping_add(offset) as i64
    }

    /// Uniform in the closed interval `[lo, hi]` for unsigned values. Requires `lo <= hi`.
    pub fn range_u(&mut self, lo: u64, hi: u64) -> u64 {
        let width = hi - lo;
        lo + if width == u64::MAX { self.next_u64() } else { self.below(width + 1) }
    }

    pub fn coin(&mut self) -> bool {
        (self.next_u64() >> 63) != 0
    }

    /// True with probability `numerator / denominator`. Requires `denominator > 0`.
    pub fn chance(&mut self, numerator: u64, denominator: u64) -> bool {
        self.below(denominator) < numerator
    }

    /// Requires a non-empty slice.
    pub fn pick<'a, T>(&mut self, items: &'a [T]) -> &'a T {
        &items[self.below(items.len() as u64) as usize]
    }

    /// An independent stream derived from this one.
    #[must_use]
    pub fn split(&mut self) -> Gen {
        Gen::new(self.next_u64())
    }
}

pub const DEFAULT_PROPERTY_SEED: u64 = 0x6A61_7276_6973_0000;
pub const DEFAULT_PROPERTY_ITERATIONS: u64 = 256;

/// How a property run is configured, from the environment.
#[derive(Clone, Copy, Debug)]
pub struct PropertyConfig {
    pub seed: u64,
    pub iterations: u64,
    pub single_case: Option<u64>,
}

fn env_u64(name: &str) -> Option<u64> {
    let text = std::env::var(name).ok()?;
    if text.is_empty() {
        return None;
    }
    if let Some(hex) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).ok()
    } else {
        text.parse().ok()
    }
}

impl PropertyConfig {
    #[must_use]
    pub fn from_env() -> Self {
        Self {
            seed: env_u64("JARVIS_PROP_SEED").unwrap_or(DEFAULT_PROPERTY_SEED),
            iterations: env_u64("JARVIS_PROP_ITERS").unwrap_or(DEFAULT_PROPERTY_ITERATIONS),
            single_case: env_u64("JARVIS_PROP_CASE"),
        }
    }
}

/// Seed of case `index` under base seed `seed`. Cases are independent of each other, so a single
/// case can be rerun without replaying the ones before it.
#[must_use]
pub fn case_seed(seed: u64, index: u64) -> u64 {
    Gen::new(seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15)).next_u64()
}

/// Runs `body` once per case. A panic in the body is re-raised with the base seed and case index
/// so the failure can be replayed with `JARVIS_PROP_SEED` and `JARVIS_PROP_CASE`.
pub fn for_all(body: impl Fn(&mut Gen)) {
    let config = PropertyConfig::from_env();
    let (first, last) = match config.single_case {
        Some(index) => (index, index + 1),
        None => (0, config.iterations),
    };
    for index in first..last {
        let mut gen = Gen::new(case_seed(config.seed, index));
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| body(&mut gen)));
        if let Err(payload) = outcome {
            eprintln!(
                "property case failed: rerun with JARVIS_PROP_SEED={:#x} JARVIS_PROP_CASE={index}",
                config.seed
            );
            std::panic::resume_unwind(payload);
        }
    }
}

thread_local! {
    static ALLOCATIONS: Cell<u64> = const { Cell::new(0) };
}

/// A global allocator that counts the calling thread's allocations. Installed by a test binary
/// with [`install_counting_allocator!`]; the count is read through [`AllocationScope`].
#[derive(Debug, Default)]
pub struct CountingAlloc;

// GlobalAlloc's methods are `unsafe fn`; forwarding to System needs no unsafe block of its own
// (the obligations are the caller's), so the crate stays `forbid(unsafe_code)`.
#[allow(unsafe_code)]
unsafe impl GlobalAlloc for CountingAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let _ = ALLOCATIONS.try_with(|c| c.set(c.get() + 1));
        // SAFETY: same contract as the caller's.
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: same contract as the caller's.
        unsafe { System.dealloc(ptr, layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        let _ = ALLOCATIONS.try_with(|c| c.set(c.get() + 1));
        // SAFETY: same contract as the caller's.
        unsafe { System.realloc(ptr, layout, new_size) }
    }
}

/// Installs [`CountingAlloc`] as the test binary's global allocator.
#[macro_export]
macro_rules! install_counting_allocator {
    () => {
        #[global_allocator]
        static JARVIS_COUNTING_ALLOCATOR: $crate::CountingAlloc = $crate::CountingAlloc;
    };
}

/// Allocations made by the current thread so far (0 when the counting allocator is not
/// installed).
#[must_use]
pub fn thread_allocation_count() -> u64 {
    ALLOCATIONS.with(Cell::get)
}

/// Counts allocations made by the current thread while the scope is alive. The zero-allocation
/// gate asserts that `allocations()` is zero after running a kernel step.
#[derive(Debug)]
pub struct AllocationScope {
    start: u64,
}

impl AllocationScope {
    #[must_use]
    pub fn new() -> Self {
        Self { start: thread_allocation_count() }
    }
    #[must_use]
    pub fn allocations(&self) -> u64 {
        thread_allocation_count() - self.start
    }
}

impl Default for AllocationScope {
    fn default() -> Self {
        Self::new()
    }
}
