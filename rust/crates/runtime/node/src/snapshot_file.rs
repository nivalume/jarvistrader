//! `EngineState` snapshot files (docs/architecture.md section 16.3): `snapshot-<seq>.jsnap` in
//! the run directory, one per snapshot point, written whole or not at all (a temporary file,
//! renamed).
//!
//! ```text
//! magic "JARVISSN" | format u16 | the run's log header | seq u64 | ts u64 | complete u8 |
//! strategies u16 | body_len u64 | body | crc32c u32 (over everything before)
//! ```
//!
//! `seq` and `ts` are those of the input after which the state was taken (a `BatchEnd`); the body
//! is `Engine::save_state`. `complete` says every strategy's own state is in it, so restoring it
//! continues exactly.

use std::path::{Path, PathBuf};

use kernel_core::crc32c::crc32c;
use kernel_core::{Result, Status};
use model::log::LogHeader;
use model::wire::{WireReader, WireWriter};
use sys::{DurableFile, Mode};

use crate::build_info;
use crate::NodeError;

pub const SNAPSHOT_FORMAT: u16 = 1;
const MAGIC: &[u8; 8] = b"JARVISSN";
const PREFIX: &str = "snapshot-";
const SUFFIX: &str = ".jsnap";

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnapshotInfo {
    pub seq: u64,
    pub ts: u64,
    pub complete: bool,
    pub strategies: u16,
}

/// `snapshot-<seq, 20 digits>.jsnap`.
#[must_use]
pub fn snapshot_name(seq: u64) -> String {
    format!("{PREFIX}{seq:020}{SUFFIX}")
}

#[must_use]
pub fn encode_snapshot(header: &LogHeader, info: &SnapshotInfo, body: &[u8]) -> Vec<u8> {
    let mut w = WireWriter::with_capacity(body.len() + 512);
    w.bytes(MAGIC);
    w.u16(SNAPSHOT_FORMAT);
    header.encode_to(&mut w);
    w.u64(info.seq);
    w.u64(info.ts);
    w.u8(u8::from(info.complete));
    w.u16(info.strategies);
    w.u64(body.len() as u64);
    w.bytes(body);
    let crc = crc32c(w.as_slice());
    w.u32(crc);
    w.into_vec()
}

/// The header, the info and the body's range within `file`.
pub fn decode_snapshot(file: &[u8]) -> Result<(LogHeader, SnapshotInfo, std::ops::Range<usize>)> {
    if file.len() < MAGIC.len() + 2 + 4 {
        return Err(Status::Truncated);
    }
    let (content, crc_bytes) = file.split_at(file.len() - 4);
    let crc = u32::from_le_bytes([crc_bytes[0], crc_bytes[1], crc_bytes[2], crc_bytes[3]]);
    if crc != crc32c(content) {
        return Err(Status::ChecksumMismatch);
    }
    let mut r = WireReader::new(content);
    if r.bytes(MAGIC.len())? != MAGIC {
        return Err(Status::InvalidArgument);
    }
    if r.u16()? != SNAPSHOT_FORMAT {
        return Err(Status::UnsupportedMessage);
    }
    let header = LogHeader::decode_from(&mut r)?;
    let seq = r.u64()?;
    let ts = r.u64()?;
    let complete = r.u8()?;
    let strategies = r.u16()?;
    let length = r.u64()? as usize;
    if complete > 1 || length != r.remaining() {
        return Err(Status::InvalidArgument);
    }
    let start = r.position();
    Ok((
        header,
        SnapshotInfo { seq, ts, complete: complete == 1, strategies },
        start..start + length,
    ))
}

/// Writes the encoded snapshot as `directory/snapshot_name(seq)` through a temporary file.
pub fn write_snapshot_file(
    directory: &Path,
    seq: u64,
    encoded: &[u8],
    durable: bool,
) -> Result<()> {
    let path = directory.join(snapshot_name(seq));
    let mut temp = path.clone().into_os_string();
    temp.push(".tmp");
    let temp = PathBuf::from(temp);
    let written = (|| {
        let mut file = DurableFile::open(&temp, Mode::Truncate)?;
        file.write_all(encoded)?;
        if durable {
            file.sync()?;
        }
        drop(file);
        sys::replace_file(&temp, &path)
    })();
    if written.is_err() {
        let _ = std::fs::remove_file(&temp);
        return written;
    }
    if durable {
        sys::sync_directory(directory)?;
    }
    Ok(())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SnapshotEntry {
    pub seq: u64,
    pub path: PathBuf,
}

/// The snapshot files of `directory`, by seq.
#[must_use]
pub fn list_snapshots(directory: &Path) -> Vec<SnapshotEntry> {
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(directory) else { return out };
    for entry in entries.flatten() {
        let path = entry.path();
        let Some(name) = path.file_name().and_then(|n| n.to_str()) else { continue };
        let Some(digits) = name.strip_prefix(PREFIX).and_then(|n| n.strip_suffix(SUFFIX)) else {
            continue;
        };
        if let Ok(seq) = digits.parse::<u64>() {
            out.push(SnapshotEntry { seq, path });
        }
    }
    out.sort_by_key(|e| e.seq);
    out
}

/// A snapshot file read whole and decoded.
#[derive(Clone, Debug)]
pub struct LoadedSnapshot {
    pub header: LogHeader,
    pub info: SnapshotInfo,
    pub file: Vec<u8>,
    pub body: std::ops::Range<usize>,
}

impl LoadedSnapshot {
    #[must_use]
    pub fn body(&self) -> &[u8] {
        &self.file[self.body.clone()]
    }
}

/// Reads `path` for a restore. It must decode, be complete (every strategy's state in it), and
/// come from this build: the state layout is the build's.
pub fn load_snapshot(path: &Path) -> std::result::Result<LoadedSnapshot, NodeError> {
    let shown = path.display();
    let file = sys::read_bytes(path)
        .map_err(|s| NodeError::new(s, format!("{shown}: cannot read the snapshot: {s}")))?;
    let (header, info, body) = decode_snapshot(&file)
        .map_err(|s| NodeError::new(s, format!("{shown}: cannot read the snapshot: {s}")))?;
    if !info.complete {
        return Err(NodeError::new(
            Status::InvalidState,
            format!(
                "{shown}: the snapshot lacks the state of a strategy that does not describe it"
            ),
        ));
    }
    let build = build_info::build_text();
    if header.build.as_str() != build {
        return Err(NodeError::new(
            Status::InvalidState,
            format!(
                "{shown}: the snapshot was written by build \"{}\" and this is \"{build}\"; a state is only restored by the build that wrote it",
                header.build.as_str()
            ),
        ));
    }
    Ok(LoadedSnapshot { header, info, file, body })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnapshotPick {
    Earliest,
    Latest,
}

/// The earliest (or latest) snapshot of `directory` with `seq >= min_seq` that `load_snapshot`
/// accepts.
#[must_use]
pub fn pick_snapshot(directory: &Path, min_seq: u64, pick: SnapshotPick) -> Option<SnapshotEntry> {
    let mut all = list_snapshots(directory);
    if pick == SnapshotPick::Latest {
        all.reverse();
    }
    all.into_iter().filter(|e| e.seq >= min_seq).find(|e| load_snapshot(&e.path).is_ok())
}
