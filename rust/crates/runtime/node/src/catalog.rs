//! Writing catalog logs: one stream's records into
//! `{catalog}/{instrument_id}/{stream}/{day}/events-000000.jlog`. The converters that turn
//! Binance archives into catalogs live in the Python tree; this is what the golden catalog
//! (`corpus::golden`) and the tests use.

use std::path::Path;

use kernel_core::sha256::Sha256;
use kernel_core::{EventKey, Result};
use model::log::LogHeader;
use model::{Event, InstrumentId};

use crate::event_log::{EventLogOptions, EventLogWriter};
use crate::log_source::catalog_directory;

/// The header catalog logs carry: no configuration, no seed, the stream as the node tag.
pub fn catalog_header(stream: &str) -> Result<LogHeader> {
    let tag: String = stream.chars().take(20).collect();
    LogHeader::new(Sha256::digest(b"jarvis-catalog"), 0, &tag, "catalog")
}

/// Writes `records` (in key order) as the log of `stream` for `instrument` on `day`.
pub fn write_stream(
    catalog: &Path,
    instrument: &InstrumentId,
    stream: &str,
    day: &str,
    records: &[(EventKey, Event)],
) -> Result<()> {
    let dir = catalog_directory(&catalog.to_string_lossy(), instrument, stream, day);
    let header = catalog_header(stream)?;
    let mut writer = EventLogWriter::open(&dir, &header, EventLogOptions::default())?;
    for (key, event) in records {
        writer.append(*key, event)?;
    }
    writer.close()
}
