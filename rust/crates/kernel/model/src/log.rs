//! The event log (docs/architecture.md section 16.1): a header, then records. A record is an
//! input event with its [`EventKey`], framed and checksummed so that a truncated or corrupted
//! tail is detected and the records before it stay readable. The log is the WAL and the
//! determinism trace at once; segment files, rolling and `fdatasync` belong to the node.
//!
//! Record layout, little-endian:
//!
//! | field | bytes | |
//! | --- | --- | --- |
//! | `len` | 4 | bytes that follow, up to and including the checksum |
//! | `kind` | 2 | [`EventKind`] tag |
//! | `flags` | 2 | reserved, zero |
//! | `ts` `source_id` `seq` | 8 2 8 | the [`EventKey`] |
//! | body | `len - 24` | the event, [`Wire`] encoding |
//! | `crc32c` | 4 | over `kind` through body |

use alloc::vec::Vec;

use kernel_core::crc32c::{crc32c_extend, crc32c_finish, crc32c_init};
use kernel_core::sha256::{self, Sha256};
use kernel_core::{EventKey, FixedString, Result, Status};

use crate::event::{Event, EventKind, SCHEMA_VERSION};
use crate::wire::{WireReader, WireWriter};

pub const MAGIC: &[u8; 8] = b"JRVSLOG\0";
pub const FORMAT_VERSION: u16 = 1;
/// A record body larger than this is refused: nothing the kernel produces comes close, and the
/// bound keeps a corrupted length from asking for gigabytes.
pub const MAX_BODY: usize = 16 * 1024 * 1024;
const RECORD_FIXED: usize = 2 + 2 + 18 + 4; // kind, flags, key, crc

/// The log header. Strings are free text for people and tools; the fields that decide a replay
/// (`config_hash`, `seed`, schema) are exact.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LogHeader {
    pub format_version: u16,
    pub schema_version: u16,
    pub config_hash: [u8; 32],
    pub seed: u64,
    pub python_hash_seed: Option<u32>,
    pub node_tag: FixedString<20>,
    pub env: FixedString<16>,
    pub build: FixedString<96>,
    pub platform: FixedString<64>,
}

impl LogHeader {
    /// A header for a run of this schema.
    pub fn new(config_hash: [u8; 32], seed: u64, node_tag: &str, env: &str) -> Result<Self> {
        Ok(Self {
            format_version: FORMAT_VERSION,
            schema_version: SCHEMA_VERSION,
            config_hash,
            seed,
            python_hash_seed: None,
            node_tag: FixedString::from_text(node_tag)?,
            env: FixedString::from_text(env)?,
            build: FixedString::new(),
            platform: FixedString::new(),
        })
    }

    /// Writes magic, the fields, and a checksum of them.
    pub fn encode_to(&self, w: &mut WireWriter) {
        w.bytes(MAGIC);
        let start = w.len();
        w.u16(self.format_version);
        w.u16(self.schema_version);
        w.bytes(&self.config_hash);
        w.u64(self.seed);
        w.put(&self.python_hash_seed);
        w.put(&self.node_tag);
        w.put(&self.env);
        w.put(&self.build);
        w.put(&self.platform);
        let crc = crc32c_finish(crc32c_extend(crc32c_init(), &w.as_slice()[start..]));
        w.u32(crc);
    }

    /// Reads a header; `UnsupportedMessage` for another format or schema version.
    pub fn decode_from(r: &mut WireReader<'_>) -> Result<Self> {
        if r.bytes(8)? != MAGIC {
            return Err(Status::ParseError);
        }
        let start = r.position();
        let format_version = r.u16()?;
        let schema_version = r.u16()?;
        if format_version != FORMAT_VERSION || schema_version != SCHEMA_VERSION {
            return Err(Status::UnsupportedMessage);
        }
        let mut config_hash = [0u8; 32];
        config_hash.copy_from_slice(r.bytes(32)?);
        let seed = r.u64()?;
        let python_hash_seed = r.get()?;
        let node_tag = r.get()?;
        let env = r.get()?;
        let build = r.get()?;
        let platform = r.get()?;
        let end = r.position();
        let crc = r.u32()?;
        let body = &r.rest_from(start)[..end - start];
        if crc != crc32c_finish(crc32c_extend(crc32c_init(), body)) {
            return Err(Status::ChecksumMismatch);
        }
        Ok(Self {
            format_version,
            schema_version,
            config_hash,
            seed,
            python_hash_seed,
            node_tag,
            env,
            build,
            platform,
        })
    }
}

impl WireReader<'_> {
    fn rest_from(&self, start: usize) -> &[u8] {
        let whole = self.whole();
        &whole[start..]
    }
}

/// One record: the key the kernel processed it under and the event.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Record {
    pub key: EventKey,
    pub event: Event,
}

/// Appends records to a byte buffer. The node hands the buffer to the persist thread; a backtest
/// writes it to its run directory.
#[derive(Debug)]
pub struct LogWriter {
    out: WireWriter,
    records: u64,
}

impl LogWriter {
    /// Starts a log with its header.
    #[must_use]
    pub fn new(header: &LogHeader) -> Self {
        let mut out = WireWriter::new();
        header.encode_to(&mut out);
        Self { out, records: 0 }
    }

    /// Continues a log whose header is already written elsewhere (a later segment).
    #[must_use]
    pub const fn continuation() -> Self {
        Self { out: WireWriter::new(), records: 0 }
    }

    pub fn append(&mut self, key: EventKey, event: &Event) -> Result<()> {
        let len_at = self.out.len();
        self.out.u32(0); // patched below
        let start = self.out.len();
        self.out.u16(event.kind().tag());
        self.out.u16(0);
        self.out.put(&key);
        event.encode_body(&mut self.out);
        let body_len = self.out.len() - start;
        if body_len - (RECORD_FIXED - 4) > MAX_BODY {
            return Err(Status::CapacityExceeded);
        }
        let crc = crc32c_finish(crc32c_extend(crc32c_init(), &self.out.as_slice()[start..]));
        self.out.u32(crc);
        let len = (self.out.len() - start) as u32;
        self.out.patch(len_at, &len.to_le_bytes());
        self.records += 1;
        Ok(())
    }

    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        self.out.as_slice()
    }
    #[must_use]
    pub fn into_bytes(self) -> Vec<u8> {
        self.out.into_vec()
    }
    #[must_use]
    pub const fn records(&self) -> u64 {
        self.records
    }
    /// Hands out the bytes written so far and starts over (the persist thread drains in batches).
    pub fn take(&mut self) -> Vec<u8> {
        core::mem::take(&mut self.out).into_vec()
    }
}

/// Reads records from a byte slice. A short tail (a crash mid-write) is `Truncated`; a record
/// whose checksum fails is `ChecksumMismatch`; both stop the reader at the last good record.
#[derive(Debug)]
pub struct LogReader<'a> {
    reader: WireReader<'a>,
}

impl<'a> LogReader<'a> {
    /// Reads and checks the header; the records follow.
    pub fn open(bytes: &'a [u8]) -> Result<(LogHeader, Self)> {
        let mut reader = WireReader::new(bytes);
        let header = LogHeader::decode_from(&mut reader)?;
        Ok((header, Self { reader }))
    }

    /// A reader over records with no header (a later segment).
    #[must_use]
    pub const fn continuation(bytes: &'a [u8]) -> Self {
        Self { reader: WireReader::new(bytes) }
    }

    /// The next record, `None` at a clean end.
    pub fn next_record(&mut self) -> Result<Option<Record>> {
        if self.reader.remaining() == 0 {
            return Ok(None);
        }
        let len = self.reader.u32()? as usize;
        if len < RECORD_FIXED || len - RECORD_FIXED > MAX_BODY {
            return Err(Status::ParseError);
        }
        let frame = self.reader.bytes(len)?;
        let (payload, crc_bytes) = frame.split_at(len - 4);
        let crc = u32::from_le_bytes([crc_bytes[0], crc_bytes[1], crc_bytes[2], crc_bytes[3]]);
        if crc != crc32c_finish(crc32c_extend(crc32c_init(), payload)) {
            return Err(Status::ChecksumMismatch);
        }
        let mut r = WireReader::new(payload);
        let kind = EventKind::from_tag(r.u16()?)?;
        if r.u16()? != 0 {
            return Err(Status::UnsupportedMessage);
        }
        let key: EventKey = r.get()?;
        let event = Event::decode_body(kind, &mut r)?;
        r.finish()?;
        Ok(Some(Record { key, event }))
    }

    /// Bytes consumed so far.
    #[must_use]
    pub const fn position(&self) -> usize {
        self.reader.position()
    }
}

impl Iterator for LogReader<'_> {
    type Item = Result<Record>;
    fn next(&mut self) -> Option<Self::Item> {
        self.next_record().transpose()
    }
}

/// What a log's records compute to, independent of header text, segment boundaries and framing:
/// the SHA-256 over the canonical `(key, event)` encodings in order, and the count.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fingerprint {
    pub records: u64,
    pub digest: [u8; 32],
}

impl Fingerprint {
    /// Writes `"{records} {hex}"`; returns the length. `out` needs 20 + 1 + 64 bytes.
    pub fn write_text(&self, out: &mut [u8]) -> usize {
        let mut pos = 0;
        let mut n = self.records;
        let mut digits = [0u8; 20];
        let mut d = 0;
        loop {
            digits[d] = b'0' + (n % 10) as u8;
            d += 1;
            n /= 10;
            if n == 0 {
                break;
            }
        }
        for i in (0..d).rev() {
            out[pos] = digits[i];
            pos += 1;
        }
        out[pos] = b' ';
        pos += 1;
        out[pos..pos + 64].copy_from_slice(&sha256::hex(&self.digest));
        pos + 64
    }
}

impl core::fmt::Display for Fingerprint {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let mut buf = [0u8; 85];
        let n = self.write_text(&mut buf);
        f.write_str(core::str::from_utf8(&buf[..n]).unwrap_or("?"))
    }
}

/// Accumulates a [`Fingerprint`] record by record.
#[derive(Clone, Debug)]
pub struct Fingerprinter {
    hasher: Sha256,
    records: u64,
    scratch: WireWriter,
}

impl Default for Fingerprinter {
    fn default() -> Self {
        Self::new()
    }
}

impl Fingerprinter {
    #[must_use]
    pub const fn new() -> Self {
        Self { hasher: Sha256::new(), records: 0, scratch: WireWriter::new() }
    }
    pub fn add(&mut self, key: EventKey, event: &Event) {
        self.scratch.clear();
        self.scratch.put(&key);
        self.scratch.put(event);
        self.hasher.update(self.scratch.as_slice());
        self.records += 1;
    }
    #[must_use]
    pub fn finish(self) -> Fingerprint {
        Fingerprint { records: self.records, digest: self.hasher.finish() }
    }
}

/// The fingerprint of a log's bytes (header included); stops at the first bad record with its
/// error.
pub fn fingerprint(bytes: &[u8]) -> Result<Fingerprint> {
    let (_, reader) = LogReader::open(bytes)?;
    let mut fp = Fingerprinter::new();
    for record in reader {
        let record = record?;
        fp.add(record.key, &record.event);
    }
    Ok(fp.finish())
}
