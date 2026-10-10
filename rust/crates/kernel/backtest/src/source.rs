//! Where inputs come from (docs/architecture.md section 5.2): a recorded log replayed in `seq`
//! order, an in-memory list, or several sources merged in key order.

use alloc::boxed::Box;
use alloc::vec::Vec;

use kernel_core::{EventKey, Result, Status};
use model::log::{LogReader, RecordBody};
use model::Event;

/// A stream of keyed inputs; `Ok(None)` at the end.
pub trait EventSource {
    fn next(&mut self) -> Result<Option<(EventKey, Event)>>;
}

/// Inputs from memory, in the order given.
#[derive(Clone, Debug, Default)]
pub struct VecSource {
    items: Vec<(EventKey, Event)>,
    next: usize,
}

impl VecSource {
    #[must_use]
    pub fn new(items: Vec<(EventKey, Event)>) -> Self {
        Self { items, next: 0 }
    }
}

impl EventSource for VecSource {
    fn next(&mut self) -> Result<Option<(EventKey, Event)>> {
        let item = self.items.get(self.next).cloned();
        self.next += 1;
        Ok(item)
    }
}

/// The input records of an event log, in log order; output records are skipped.
#[derive(Debug)]
pub struct ReplaySource<'a> {
    reader: LogReader<'a>,
}

impl<'a> ReplaySource<'a> {
    /// Opens a log with its header.
    pub fn open(bytes: &'a [u8]) -> Result<Self> {
        let (_, reader) = LogReader::open(bytes)?;
        Ok(Self { reader })
    }
}

impl EventSource for ReplaySource<'_> {
    fn next(&mut self) -> Result<Option<(EventKey, Event)>> {
        loop {
            let Some(record) = self.reader.next_record()? else { return Ok(None) };
            if let RecordBody::Input(event) = record.body {
                return Ok(Some((record.key, event)));
            }
        }
    }
}

struct Head {
    source: Box<dyn EventSource>,
    item: Option<(EventKey, Event)>,
    last: Option<EventKey>,
    done: bool,
}

/// Merges several sources into one stream in key order. Each source must yield strictly
/// increasing keys (`InvalidArgument` otherwise); equal keys from different sources are served in
/// the order the sources were added, so the merge is deterministic even for sloppy inputs.
pub struct MergeSource {
    heads: Vec<Head>,
    primed: bool,
}

impl core::fmt::Debug for MergeSource {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("MergeSource")
            .field("sources", &self.heads.len())
            .field("primed", &self.primed)
            .finish()
    }
}

impl MergeSource {
    #[must_use]
    pub fn new(sources: Vec<Box<dyn EventSource>>) -> Self {
        Self {
            heads: sources
                .into_iter()
                .map(|source| Head { source, item: None, last: None, done: false })
                .collect(),
            primed: false,
        }
    }

    fn pull(head: &mut Head) -> Result<()> {
        if head.done || head.item.is_some() {
            return Ok(());
        }
        match head.source.next()? {
            None => head.done = true,
            Some((key, event)) => {
                if head.last.is_some_and(|last| key <= last) {
                    return Err(Status::InvalidArgument);
                }
                head.last = Some(key);
                head.item = Some((key, event));
            }
        }
        Ok(())
    }
}

impl EventSource for MergeSource {
    fn next(&mut self) -> Result<Option<(EventKey, Event)>> {
        if !self.primed {
            self.primed = true;
        }
        for head in &mut self.heads {
            Self::pull(head)?;
        }
        let mut best: Option<usize> = None;
        for (i, head) in self.heads.iter().enumerate() {
            if let Some((key, _)) = &head.item {
                if best.is_none_or(|b| *key < self.heads[b].item.as_ref().map_or(*key, |(k, _)| *k))
                {
                    best = Some(i);
                }
            }
        }
        Ok(best.and_then(|i| self.heads[i].item.take()))
    }
}
