//! Backtest inputs from the data catalog (docs/architecture.md section 16.1). A catalog holds one
//! event log per instrument, stream and UTC day:
//!
//! ```text
//! {catalog}/{instrument_id}/{stream}/{YYYY-MM-DD}/events-NNNNNN.jlog
//! ```
//!
//! A [`LogSource`] reads the days of one stream in order; the node merges the streams by key.

use std::path::{Path, PathBuf};

use backtest::EventSource;
use kernel_core::time::parse_rfc3339;
use kernel_core::{EventKey, Result, Status, UnixNanos};
use model::log::RecordBody;
use model::{Event, InstrumentId};

use crate::config::{DataSection, TimeRange};
use crate::event_log::{EventLogReadOptions, EventLogReader};
use crate::{NodeError, NodeResult};

const DAY_NS: u64 = 86_400 * 1_000_000_000;

/// Yields the input records of several logs, one directory after another; output records are
/// skipped. Each log numbers its records from 1, so a later directory's seq is offset by the
/// records already read: keys stay strictly increasing across days even when two days meet at
/// the same timestamp.
#[derive(Debug)]
pub struct LogSource {
    directories: Vec<PathBuf>,
    index: usize,
    seq_offset: u64,
    last_seq: u64,
    reader: Option<EventLogReader>,
}

impl LogSource {
    /// Opens the first directory; `NotFound` when it holds no log.
    pub fn open(directories: Vec<PathBuf>) -> Result<Self> {
        let reader = match directories.first() {
            Some(first) => Some(EventLogReader::open(first, EventLogReadOptions::default())?),
            None => None,
        };
        Ok(Self { directories, index: 0, seq_offset: 0, last_seq: 0, reader })
    }

    /// The directory being read.
    #[must_use]
    pub fn directory(&self) -> Option<&Path> {
        self.directories.get(self.index).map(PathBuf::as_path)
    }
}

impl EventSource for LogSource {
    fn next(&mut self) -> Result<Option<(EventKey, Event)>> {
        loop {
            let Some(reader) = self.reader.as_mut() else { return Ok(None) };
            match reader.next_record()? {
                None => {
                    if self.index + 1 >= self.directories.len() {
                        self.reader = None;
                        return Ok(None);
                    }
                    self.index += 1;
                    self.seq_offset += self.last_seq;
                    self.last_seq = 0;
                    match EventLogReader::open(
                        &self.directories[self.index],
                        EventLogReadOptions::default(),
                    ) {
                        Ok(r) => self.reader = Some(r),
                        Err(e) => {
                            self.reader = None;
                            return Err(e);
                        }
                    }
                }
                Some(record) => {
                    if let RecordBody::Input(event) = record.body {
                        self.last_seq = record.key.seq;
                        let key = EventKey::new(
                            record.key.ts,
                            record.key.source_id,
                            self.seq_offset + record.key.seq,
                        );
                        return Ok(Some((key, event)));
                    }
                }
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CatalogStream {
    pub instrument: InstrumentId,
    pub stream: String,
    /// Log directories, oldest first.
    pub days: Vec<PathBuf>,
}

/// `{catalog}/{instrument_id}/{stream}[/{day}]`.
#[must_use]
pub fn catalog_directory(
    catalog: &str,
    instrument: &InstrumentId,
    stream: &str,
    day: &str,
) -> PathBuf {
    let mut p = PathBuf::from(catalog);
    p.push(instrument.to_string());
    p.push(stream);
    if !day.is_empty() {
        p.push(day);
    }
    p
}

/// `YYYY-MM-DD` to the UTC midnight that starts it.
#[must_use]
pub fn day_start(day: &str) -> Option<UnixNanos> {
    if day.len() != 10 {
        return None;
    }
    parse_rfc3339(&format!("{day}T00:00:00Z")).ok()
}

fn in_range(range: Option<&TimeRange>, start: UnixNanos) -> bool {
    match range {
        None => true,
        Some(r) => {
            let end = UnixNanos::new(start.value().saturating_add(DAY_NS));
            start < r.end && r.start < end
        }
    }
}

/// The day directories under `root`, oldest first (`YYYY-MM-DD` names only).
#[must_use]
pub fn day_directories(root: &Path) -> Vec<(String, PathBuf)> {
    let mut names: Vec<String> = Vec::new();
    if let Ok(entries) = std::fs::read_dir(root) {
        for entry in entries.flatten() {
            if entry.path().is_dir() {
                names.push(entry.file_name().to_string_lossy().into_owned());
            }
        }
    }
    names.sort();
    names
        .into_iter()
        .filter(|d| day_start(d).is_some())
        .map(|d| (d.clone(), root.join(d)))
        .collect()
}

fn days_in_range(root: &Path, range: Option<&TimeRange>) -> Vec<PathBuf> {
    day_directories(root)
        .into_iter()
        .filter(|(day, _)| day_start(day).is_some_and(|start| in_range(range, start)))
        .map(|(_, path)| path)
        .collect()
}

/// The catalog directories that `[data]` selects: every configured (instrument, stream) with the
/// days that overlap `data.range` (all days without a range). A stream without any day in range
/// is an error naming the path that was searched.
pub fn resolve_catalog(data: &DataSection) -> NodeResult<Vec<CatalogStream>> {
    if data.catalog.is_empty() {
        return Err(NodeError::new(Status::InvalidArgument, "data.catalog is not set"));
    }
    let mut out = Vec::new();
    for group in &data.streams {
        for instrument in &group.instruments {
            for stream in &group.streams {
                let root = catalog_directory(&data.catalog, instrument, stream, "");
                let days = days_in_range(&root, data.range.as_ref());
                if days.is_empty() {
                    return Err(NodeError::new(
                        Status::NotFound,
                        format!(
                            "no data for {instrument} {stream} in {}{}",
                            root.display(),
                            if data.range.is_some() { " within data.range" } else { "" }
                        ),
                    ));
                }
                out.push(CatalogStream { instrument: *instrument, stream: stream.clone(), days });
            }
        }
    }
    if out.is_empty() {
        return Err(NodeError::new(Status::InvalidArgument, "[data] selects no streams"));
    }
    Ok(out)
}
