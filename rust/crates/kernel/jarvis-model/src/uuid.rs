//! RFC 4122 version 4 UUIDs for event ids. The kernel never draws randomness of its own: an id is
//! made from 128 bits the caller took from a counter-based generator, so a replay recomputes the
//! same ids.

use core::fmt;
use core::str::FromStr;

use jarvis_core::{Result, Status};

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Uuid4([u8; 16]);

impl Uuid4 {
    /// Sets the version and variant bits on `random` and keeps the other 122 bits.
    #[must_use]
    pub const fn from_random(mut random: [u8; 16]) -> Self {
        random[6] = (random[6] & 0x0F) | 0x40;
        random[8] = (random[8] & 0x3F) | 0x80;
        Self(random)
    }

    /// Two 64-bit draws, high then low.
    #[must_use]
    pub const fn from_u64s(hi: u64, lo: u64) -> Self {
        let h = hi.to_be_bytes();
        let l = lo.to_be_bytes();
        Self::from_random([
            h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], l[0], l[1], l[2], l[3], l[4], l[5],
            l[6], l[7],
        ])
    }

    /// Exactly the bytes given; `InvalidArgument` unless they carry version 4 and the RFC variant.
    pub fn from_bytes(bytes: [u8; 16]) -> Result<Self> {
        if bytes[6] >> 4 != 4 || bytes[8] >> 6 != 0b10 {
            return Err(Status::InvalidArgument);
        }
        Ok(Self(bytes))
    }

    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; 16] {
        &self.0
    }
    #[must_use]
    pub const fn is_nil(&self) -> bool {
        let mut i = 0;
        while i < 16 {
            if self.0[i] != 0 {
                return false;
            }
            i += 1;
        }
        true
    }

    /// Parses the 36-character hyphenated lower- or upper-case form.
    pub fn parse(text: &str) -> Result<Self> {
        let b = text.as_bytes();
        if b.len() != 36 {
            return Err(Status::ParseError);
        }
        let mut out = [0u8; 16];
        let mut i = 0;
        for (pos, chunk) in [(0, 8), (9, 4), (14, 4), (19, 4), (24, 12)] {
            if pos > 0 && b[pos - 1] != b'-' {
                return Err(Status::ParseError);
            }
            let mut j = pos;
            while j < pos + chunk {
                let hi = hex_value(b[j]).ok_or(Status::ParseError)?;
                let lo = hex_value(b[j + 1]).ok_or(Status::ParseError)?;
                out[i] = (hi << 4) | lo;
                i += 1;
                j += 2;
            }
        }
        Self::from_bytes(out)
    }
}

const fn hex_value(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

impl fmt::Display for Uuid4 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        const DIGITS: &[u8; 16] = b"0123456789abcdef";
        let mut out = [0u8; 36];
        let mut pos = 0;
        for (i, b) in self.0.iter().enumerate() {
            if i == 4 || i == 6 || i == 8 || i == 10 {
                out[pos] = b'-';
                pos += 1;
            }
            out[pos] = DIGITS[(b >> 4) as usize];
            out[pos + 1] = DIGITS[(b & 0x0F) as usize];
            pos += 2;
        }
        f.write_str(core::str::from_utf8(&out).unwrap_or("?"))
    }
}
impl fmt::Debug for Uuid4 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Uuid4({self})")
    }
}
impl FromStr for Uuid4 {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}
