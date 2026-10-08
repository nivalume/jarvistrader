//! jarvis's `ClientOrderId` format: `{node_tag}-{epoch}-{seq}` (docs/architecture.md section
//! 8.4). No wall clock goes into it, and it decodes, so reconciliation can tell an order of an
//! earlier epoch of this node from an order placed by someone else.
//!
//! `epoch` and `seq` are Base32 (RFC 4648 alphabet, no padding) at fixed width: six characters
//! for the 30-bit epoch, eight for the 40-bit sequence, so the id sorts in placement order and the
//! whole id stays within Binance's 36-character limit for a tag of up to 20 characters.

use kernel_core::{Result, Status};

use crate::identifiers::ClientOrderId;

const ALPHABET: &[u8; 32] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
pub const EPOCH_CHARS: usize = 6;
pub const SEQ_CHARS: usize = 8;
pub const EPOCH_MAX: u64 = (1 << 30) - 1;
pub const SEQ_MAX: u64 = (1 << 40) - 1;
pub const TAG_MAX: usize = ClientOrderId::CAPACITY - EPOCH_CHARS - SEQ_CHARS - 2;

fn encode_base32(mut value: u64, out: &mut [u8]) {
    for slot in out.iter_mut().rev() {
        *slot = ALPHABET[(value & 31) as usize];
        value >>= 5;
    }
}

fn decode_base32(text: &[u8]) -> Option<u64> {
    let mut value: u64 = 0;
    for &c in text {
        let digit = match c {
            b'A'..=b'Z' => c - b'A',
            b'2'..=b'7' => c - b'2' + 26,
            _ => return None,
        };
        value = (value << 5) | u64::from(digit);
    }
    Some(value)
}

/// `true` for a tag jarvis accepts: 1 to `TAG_MAX` ASCII letters or digits.
#[must_use]
pub fn is_valid_tag(tag: &str) -> bool {
    !tag.is_empty() && tag.len() <= TAG_MAX && tag.bytes().all(|b| b.is_ascii_alphanumeric())
}

/// Formats an id. `InvalidArgument` for a bad tag, `OutOfRange` for `epoch` or `seq` past their
/// widths.
pub fn format(node_tag: &str, epoch: u64, seq: u64) -> Result<ClientOrderId> {
    if !is_valid_tag(node_tag) {
        return Err(Status::InvalidArgument);
    }
    if epoch > EPOCH_MAX || seq > SEQ_MAX {
        return Err(Status::OutOfRange);
    }
    let mut buf = [0u8; ClientOrderId::CAPACITY];
    let tag = node_tag.as_bytes();
    buf[..tag.len()].copy_from_slice(tag);
    let mut pos = tag.len();
    buf[pos] = b'-';
    pos += 1;
    encode_base32(epoch, &mut buf[pos..pos + EPOCH_CHARS]);
    pos += EPOCH_CHARS;
    buf[pos] = b'-';
    pos += 1;
    encode_base32(seq, &mut buf[pos..pos + SEQ_CHARS]);
    pos += SEQ_CHARS;
    let text = core::str::from_utf8(&buf[..pos]).map_err(|_| Status::InvalidArgument)?;
    ClientOrderId::new(text)
}

/// The parts of an id in jarvis's format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Decoded<'a> {
    pub node_tag: &'a str,
    pub epoch: u64,
    pub seq: u64,
}

/// Decodes an id; `ParseError` for one that is not in jarvis's format (an `EXTERNAL` order, an
/// id another system placed).
pub fn decode(id: &ClientOrderId) -> Result<Decoded<'_>> {
    let text = id.as_str();
    let (head, seq_text) = text.rsplit_once('-').ok_or(Status::ParseError)?;
    let (tag, epoch_text) = head.rsplit_once('-').ok_or(Status::ParseError)?;
    if !is_valid_tag(tag) || epoch_text.len() != EPOCH_CHARS || seq_text.len() != SEQ_CHARS {
        return Err(Status::ParseError);
    }
    let epoch = decode_base32(epoch_text.as_bytes()).ok_or(Status::ParseError)?;
    let seq = decode_base32(seq_text.as_bytes()).ok_or(Status::ParseError)?;
    Ok(Decoded { node_tag: tag, epoch, seq })
}

/// Hands out the ids of one epoch in order. The epoch is persisted by the node before any order
/// carries it (docs/architecture.md section 4.1), so two runs never share an id.
#[derive(Clone, Debug)]
pub struct Generator {
    tag: kernel_core::FixedString<20>,
    epoch: u64,
    next_seq: u64,
}

impl Generator {
    pub fn new(node_tag: &str, epoch: u64) -> Result<Self> {
        if !is_valid_tag(node_tag) {
            return Err(Status::InvalidArgument);
        }
        if epoch > EPOCH_MAX {
            return Err(Status::OutOfRange);
        }
        Ok(Self { tag: kernel_core::FixedString::from_text(node_tag)?, epoch, next_seq: 1 })
    }

    /// The next id; `OutOfRange` once the epoch's 2^40 ids are used.
    pub fn next_id(&mut self) -> Result<ClientOrderId> {
        if self.next_seq > SEQ_MAX {
            return Err(Status::OutOfRange);
        }
        let id = format(self.tag.as_str(), self.epoch, self.next_seq)?;
        self.next_seq += 1;
        Ok(id)
    }

    /// The id with a given sequence number in this epoch (for recovery and tests).
    pub fn id_of(&self, seq: u64) -> Result<ClientOrderId> {
        format(self.tag.as_str(), self.epoch, seq)
    }

    #[must_use]
    pub const fn epoch(&self) -> u64 {
        self.epoch
    }
    #[must_use]
    pub const fn next_seq(&self) -> u64 {
        self.next_seq
    }
    #[must_use]
    pub fn node_tag(&self) -> &str {
        self.tag.as_str()
    }

    /// Resumes after `seq` (recovery from a log that shows `seq` as the last issued).
    pub fn resume_after(&mut self, seq: u64) -> Result<()> {
        if seq > SEQ_MAX {
            return Err(Status::OutOfRange);
        }
        self.next_seq = seq + 1;
        Ok(())
    }
}

kernel_core::state_fields!(Generator { tag, epoch, next_seq });
