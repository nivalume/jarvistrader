//! Counter-based random numbers (ADR 0001, decision 4). A draw is a pure function of
//! `(seed, identity, hop, index)`, so results never depend on call order and any draw can be
//! recomputed in isolation.

/// splitmix64 finalizer: a bijective 64-bit mix.
#[must_use]
pub const fn mix64(mut z: u64) -> u64 {
    z = z.wrapping_add(0x9E37_79B9_7F4A_7C15);
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

pub type PhiloxCounter = [u32; 4];
pub type PhiloxKey = [u32; 2];

/// Philox4x32-10 (Salmon et al., "Parallel random numbers: as easy as 1, 2, 3", SC11).
#[must_use]
pub const fn philox4x32_10(mut ctr: PhiloxCounter, mut key: PhiloxKey) -> PhiloxCounter {
    const M0: u64 = 0xD251_1F53;
    const M1: u64 = 0xCD9E_8D57;
    const W0: u32 = 0x9E37_79B9;
    const W1: u32 = 0xBB67_AE85;
    let mut round = 0;
    while round < 10 {
        if round > 0 {
            key[0] = key[0].wrapping_add(W0);
            key[1] = key[1].wrapping_add(W1);
        }
        let p0 = M0 * ctr[0] as u64;
        let p1 = M1 * ctr[2] as u64;
        let hi0 = (p0 >> 32) as u32;
        let lo0 = p0 as u32;
        let hi1 = (p1 >> 32) as u32;
        let lo1 = p1 as u32;
        ctr = [hi1 ^ ctr[1] ^ key[0], lo1, hi0 ^ ctr[3] ^ key[1], lo0];
        round += 1;
    }
    ctr
}

/// Draws keyed by `(seed, identity, hop, index)`. `identity` names the thing being randomized (an
/// order, an event sequence number), `hop` the purpose (outbound latency, inbound latency, ...),
/// and `index` distinguishes several draws for the same purpose.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CounterRng {
    key: PhiloxKey,
}

impl CounterRng {
    #[must_use]
    pub const fn new(seed: u64) -> Self {
        Self { key: [seed as u32, (seed >> 32) as u32] }
    }

    #[must_use]
    pub const fn draw(&self, identity: u64, hop: u32, index: u32) -> u64 {
        let out = philox4x32_10([identity as u32, (identity >> 32) as u32, hop, index], self.key);
        ((out[1] as u64) << 32) | out[0] as u64
    }

    /// Uniform in `[0, bound)` by multiply-high. Bias is at most `bound / 2^64`, far below
    /// anything a latency model can observe; requires `bound > 0`.
    #[must_use]
    pub const fn below(&self, bound: u64, identity: u64, hop: u32, index: u32) -> u64 {
        let product = (self.draw(identity, hop, index) as u128) * (bound as u128);
        (product >> 64) as u64
    }
}
