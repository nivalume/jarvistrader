//! CRC-32C (Castagnoli), as used for event log records.
//!
//! The C++ tree uses the SSE4.2 and `ARMv8` CRC instructions when available; this crate is
//! `forbid(unsafe_code)` and the intrinsics are unsafe, so it uses slicing-by-8 tables instead.
//! Same polynomial and bit order, so the checksums are identical; only speed differs. A
//! hardware path can live in the shell behind the same three functions.

const POLY: u32 = 0x82F6_3B78;

const fn make_tables() -> [[u32; 256]; 8] {
    let mut tables = [[0u32; 256]; 8];
    let mut i = 0;
    while i < 256 {
        let mut crc = i as u32;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ POLY } else { crc >> 1 };
            bit += 1;
        }
        tables[0][i] = crc;
        i += 1;
    }
    let mut t = 1;
    while t < 8 {
        let mut i = 0;
        while i < 256 {
            let prev = tables[t - 1][i];
            tables[t][i] = tables[0][(prev & 0xFF) as usize] ^ (prev >> 8);
            i += 1;
        }
        t += 1;
    }
    tables
}

static TABLES: [[u32; 256]; 8] = make_tables();

/// The running state before any data.
#[must_use]
pub const fn crc32c_init() -> u32 {
    0xFFFF_FFFF
}

/// Continues a running checksum over `data`.
#[must_use]
pub fn crc32c_extend(mut state: u32, data: &[u8]) -> u32 {
    let mut chunks = data.chunks_exact(8);
    for chunk in &mut chunks {
        let word = u64::from_le_bytes([
            chunk[0], chunk[1], chunk[2], chunk[3], chunk[4], chunk[5], chunk[6], chunk[7],
        ]) ^ u64::from(state);
        state = TABLES[7][(word & 0xFF) as usize]
            ^ TABLES[6][((word >> 8) & 0xFF) as usize]
            ^ TABLES[5][((word >> 16) & 0xFF) as usize]
            ^ TABLES[4][((word >> 24) & 0xFF) as usize]
            ^ TABLES[3][((word >> 32) & 0xFF) as usize]
            ^ TABLES[2][((word >> 40) & 0xFF) as usize]
            ^ TABLES[1][((word >> 48) & 0xFF) as usize]
            ^ TABLES[0][((word >> 56) & 0xFF) as usize];
    }
    crc32c_bytewise(state, chunks.remainder())
}

/// The reference byte-at-a-time form of [`crc32c_extend`]; tests check the two agree.
#[must_use]
pub fn crc32c_bytewise(mut state: u32, data: &[u8]) -> u32 {
    for &b in data {
        state = TABLES[0][((state ^ u32::from(b)) & 0xFF) as usize] ^ (state >> 8);
    }
    state
}

/// Finishes a running checksum.
#[must_use]
pub const fn crc32c_finish(state: u32) -> u32 {
    state ^ 0xFFFF_FFFF
}

/// The CRC-32C of `data` in one call.
#[must_use]
pub fn crc32c(data: &[u8]) -> u32 {
    crc32c_finish(crc32c_extend(crc32c_init(), data))
}
