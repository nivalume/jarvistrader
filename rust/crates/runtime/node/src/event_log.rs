//! The run log on disk (docs/architecture.md section 16.1): segments named `events-000000.jlog`,
//! `events-000001.jlog`, ... in the run directory. Each segment starts with the run's
//! [`LogHeader`]; records follow back to back in `model::log` framing. A log whose older segments
//! were removed after a snapshot starts at a later index; its segments are still consecutive.

use std::path::{Path, PathBuf};

use kernel_core::{EventKey, Result, Status};
use model::log::{LogHeader, LogReader, LogWriter, Record};
use model::outputs::Output;
use model::wire::WireWriter;
use model::Event;
use sys::{DurableFile, Mode};

use crate::snapshot_file::{self, SnapshotInfo};

const FLUSH_THRESHOLD: usize = 1 << 20;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EventLogOptions {
    /// Roll to a new segment beyond this size.
    pub segment_bytes: u64,
    /// `fdatasync` after each flush.
    pub sync_on_flush: bool,
    /// A finished segment is synced before it is closed, and the directory after a segment is
    /// created, so that what `sync` made durable stays reachable after a crash.
    pub durable: bool,
}

impl Default for EventLogOptions {
    fn default() -> Self {
        Self { segment_bytes: 256 << 20, sync_on_flush: false, durable: false }
    }
}

/// `events-NNNNNN.jlog`.
#[must_use]
pub fn segment_name(index: u32) -> String {
    format!("events-{index:06}.jlog")
}

/// Whether `directory` holds a run log.
#[must_use]
pub fn has_log(directory: &Path) -> bool {
    segments_of(directory).is_ok_and(|s| !s.is_empty())
}

fn segments_of(directory: &Path) -> std::io::Result<Vec<PathBuf>> {
    let mut out = Vec::new();
    for entry in std::fs::read_dir(directory)? {
        let path = entry?.path();
        if path.extension().is_some_and(|e| e == "jlog") {
            out.push(path);
        }
    }
    out.sort();
    Ok(out)
}

/// Writes the segments of a run log.
#[derive(Debug)]
pub struct EventLogWriter {
    file: Option<DurableFile>,
    directory: PathBuf,
    header: LogHeader,
    options: EventLogOptions,
    segment_index: u32,
    segment_size: u64,
    records: u64,
    position: u64,
    buffer: Vec<u8>,
    scratch: LogWriter,
}

impl EventLogWriter {
    /// Creates `directory` if needed; `AlreadyExists` when it holds a log.
    pub fn open(directory: &Path, header: &LogHeader, options: EventLogOptions) -> Result<Self> {
        std::fs::create_dir_all(directory).map_err(|_| Status::IoError)?;
        if has_log(directory) {
            return Err(Status::AlreadyExists);
        }
        let mut writer = Self {
            file: None,
            directory: directory.to_path_buf(),
            header: header.clone(),
            options,
            segment_index: 0,
            segment_size: 0,
            records: 0,
            position: 0,
            buffer: Vec::with_capacity(FLUSH_THRESHOLD + 4096),
            scratch: LogWriter::continuation(),
        };
        writer.open_segment()?;
        Ok(writer)
    }

    fn open_segment(&mut self) -> Result<()> {
        let path = self.directory.join(segment_name(self.segment_index));
        self.file = Some(DurableFile::open(&path, Mode::CreateNew)?);
        if self.options.durable {
            sys::sync_directory(&self.directory)?;
        }
        let mut w = WireWriter::new();
        self.header.encode_to(&mut w);
        self.segment_size = w.len() as u64;
        self.buffer.extend_from_slice(w.as_slice());
        Ok(())
    }

    pub fn append(&mut self, key: EventKey, event: &Event) -> Result<()> {
        self.scratch.append(key, event)?;
        let record = self.scratch.take();
        self.append_record(&record)
    }

    /// A kernel output; `key` is the causing input's key with `source_id` = output index.
    pub fn append_output(&mut self, key: EventKey, output: &Output) -> Result<()> {
        self.scratch.append_output(key, output)?;
        let record = self.scratch.take();
        self.append_record(&record)
    }

    /// An already framed record.
    pub fn append_record(&mut self, record: &[u8]) -> Result<()> {
        if self.file.is_none() {
            return Err(Status::InvalidState);
        }
        if self.segment_size + record.len() as u64 > self.options.segment_bytes && self.records > 0
        {
            self.flush()?;
            if self.options.durable && !self.options.sync_on_flush {
                self.sync_fd()?;
            }
            self.file = None;
            self.segment_index += 1;
            self.open_segment()?;
        }
        self.buffer.extend_from_slice(record);
        self.segment_size += record.len() as u64;
        self.position += record.len() as u64;
        self.records += 1;
        if self.buffer.len() >= FLUSH_THRESHOLD {
            self.flush()?;
        }
        Ok(())
    }

    /// Writes an `EngineState` snapshot file beside the segments.
    pub fn write_snapshot(&self, info: &SnapshotInfo, body: &[u8]) -> Result<()> {
        let encoded = snapshot_file::encode_snapshot(&self.header, info, body);
        snapshot_file::write_snapshot_file(
            &self.directory,
            info.seq,
            &encoded,
            self.options.durable,
        )
    }

    pub fn flush(&mut self) -> Result<()> {
        let file = self.file.as_mut().ok_or(Status::InvalidState)?;
        file.write_all(&self.buffer)?;
        self.buffer.clear();
        if self.options.sync_on_flush {
            file.sync_data()?;
        }
        Ok(())
    }

    fn sync_fd(&mut self) -> Result<()> {
        self.file.as_mut().ok_or(Status::InvalidState)?.sync_data()
    }

    /// `flush` and `fdatasync`: every record appended so far survives a crash.
    pub fn sync(&mut self) -> Result<()> {
        self.flush()?;
        if !self.options.sync_on_flush {
            self.sync_fd()?;
        }
        Ok(())
    }

    pub fn close(&mut self) -> Result<()> {
        if self.file.is_none() {
            return Ok(());
        }
        self.flush()?;
        if self.options.durable && !self.options.sync_on_flush {
            self.sync_fd()?;
        }
        self.file = None;
        Ok(())
    }

    #[must_use]
    pub const fn records(&self) -> u64 {
        self.records
    }
    /// Bytes of records appended so far, across segments (headers not counted).
    #[must_use]
    pub const fn position(&self) -> u64 {
        self.position
    }
    #[must_use]
    pub const fn segment_index(&self) -> u32 {
        self.segment_index
    }
    #[must_use]
    pub fn directory(&self) -> &Path {
        &self.directory
    }
}

impl Drop for EventLogWriter {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EventLogReadOptions {
    /// A log a crash interrupted ends in a partial record. With this set, the first record of
    /// the last segment that does not decode ends the log instead of failing it; `torn_bytes`
    /// tells how much was left out.
    pub tolerate_torn_tail: bool,
}

/// Reads the records of a run log, segment after segment.
#[derive(Debug)]
pub struct EventLogReader {
    options: EventLogReadOptions,
    segments: Vec<PathBuf>,
    segment: usize,
    data: Vec<u8>,
    pos: usize,
    header: LogHeader,
    torn_bytes: u64,
}

impl EventLogReader {
    /// `NotFound` when `directory` holds no log.
    pub fn open(directory: &Path, options: EventLogReadOptions) -> Result<Self> {
        let segments = segments_of(directory).map_err(|_| Status::IoError)?;
        if segments.is_empty() {
            return Err(Status::NotFound);
        }
        let mut reader = Self {
            options,
            segments,
            segment: 0,
            data: Vec::new(),
            pos: 0,
            header: LogHeader::new([0; 32], 0, "", "")?,
            torn_bytes: 0,
        };
        reader.load_segment(0)?;
        Ok(reader)
    }

    /// The header of the first segment; later segments must carry the same one.
    #[must_use]
    pub const fn header(&self) -> &LogHeader {
        &self.header
    }
    #[must_use]
    pub fn segments(&self) -> &[PathBuf] {
        &self.segments
    }
    /// With `tolerate_torn_tail`, once the log ended: bytes after the last whole record.
    #[must_use]
    pub const fn torn_bytes(&self) -> u64 {
        self.torn_bytes
    }

    fn last_segment(&self) -> bool {
        self.segment + 1 >= self.segments.len()
    }

    fn load_segment(&mut self, index: usize) -> Result<()> {
        self.data = sys::read_bytes(&self.segments[index]).map_err(|_| Status::IoError)?;
        self.segment = index;
        match LogReader::open(&self.data) {
            Ok((header, reader)) => {
                if index == 0 {
                    self.header = header;
                } else if header != self.header {
                    return Err(Status::InvalidArgument);
                }
                self.pos = reader.position();
                Ok(())
            }
            Err(e) => {
                if self.options.tolerate_torn_tail && self.last_segment() {
                    self.torn(0);
                    return Ok(());
                }
                Err(e)
            }
        }
    }

    fn torn(&mut self, from: usize) {
        self.torn_bytes = (self.data.len() - from) as u64;
        self.pos = self.data.len();
    }

    /// The next record in log order; `None` at the end.
    pub fn next_record(&mut self) -> Result<Option<Record>> {
        while self.pos >= self.data.len() {
            if self.last_segment() {
                return Ok(None);
            }
            self.load_segment(self.segment + 1)?;
        }
        let mut reader = LogReader::continuation(&self.data[self.pos..]);
        match reader.next_record() {
            Ok(Some(record)) => {
                self.pos += reader.position();
                Ok(Some(record))
            }
            Ok(None) => Ok(None),
            Err(e) => {
                if self.options.tolerate_torn_tail && self.last_segment() {
                    self.torn(self.pos);
                    return Ok(None);
                }
                Err(e)
            }
        }
    }
}

impl Iterator for EventLogReader {
    type Item = Result<Record>;
    fn next(&mut self) -> Option<Self::Item> {
        self.next_record().transpose()
    }
}
