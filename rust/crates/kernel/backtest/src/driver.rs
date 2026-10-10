//! The backtest step loop (docs/architecture.md sections 4.3 and 5.4). It turns a stream of data
//! events into the node's input sequence and steps the engine through it:
//!
//! - lifecycle: `Init -> Wired -> Starting -> Syncing -> Running` before the first data event,
//!   `Running -> Stopping -> Stopped` after the last one (or when a strategy error halts the node);
//! - timers: before each data event, every timer due at or before its `ts` fires as `TimerFired`;
//! - batches: a batch is a maximal run of inputs with the same `ts`, closed by `BatchEnd`.
//!
//! With a venue loop as the source (section 12.2) the driver also runs the simulated venue: it
//! always processes the earliest of the next venue-side event (market data the venue matches on,
//! a command arriving), the next timer and the next kernel input (delayed market data, a venue
//! answer), in that order on a tie, and hands the commands of every step to the venue loop.
//!
//! Every input the driver steps is recorded first, with the key `(ts, source_id, seq)`: `seq` is
//! the driver's own counter from 1, `source_id` the data source's, or [`KERNEL_SOURCE`] for the
//! inputs the driver synthesises. The engine's outputs follow their input. The recording is
//! therefore a complete run log: replaying it reproduces every call and output without this
//! driver ([`crate::replay`]).

use alloc::vec::Vec;

use data::subscription::StrategyIndex;
use engine::lifecycle::{is_terminal, Lifecycle};
use engine::Engine;
use kernel_core::{EventKey, Result, UnixNanos};
use model::enums::{LifecycleReason, NodeState};
use model::event::{BatchEnd, TimerFired};
use model::log::{Fingerprint, Fingerprinter, LogWriter};
use model::outputs::Output;
use model::Event;

use crate::source::EventSource;
use crate::venue_loop::{VenueLoop, VENUE_SOURCE};

pub const KERNEL_SOURCE: u16 = 0;

/// Receives every input before it is stepped and every output after.
pub trait Recorder {
    fn record(&mut self, key: EventKey, event: &Event) -> Result<()>;
    fn emit(&mut self, key: EventKey, output: &Output) -> Result<()>;
}

/// Records nothing but the fingerprint.
#[derive(Debug, Default)]
pub struct FingerprintRecorder {
    fp: Fingerprinter,
}

impl FingerprintRecorder {
    #[must_use]
    pub fn finish(self) -> Fingerprint {
        self.fp.finish()
    }
}

impl Recorder for FingerprintRecorder {
    fn record(&mut self, key: EventKey, event: &Event) -> Result<()> {
        self.fp.add(key, event);
        Ok(())
    }
    fn emit(&mut self, key: EventKey, output: &Output) -> Result<()> {
        self.fp.add_output(key, output);
        Ok(())
    }
}

/// Writes the run log.
#[derive(Debug)]
pub struct LogRecorder {
    pub writer: LogWriter,
    pub fp: Fingerprinter,
}

impl LogRecorder {
    #[must_use]
    pub fn new(writer: LogWriter) -> Self {
        Self { writer, fp: Fingerprinter::new() }
    }
    #[must_use]
    pub fn finish(self) -> (Vec<u8>, Fingerprint) {
        (self.writer.into_bytes(), self.fp.finish())
    }
}

impl Recorder for LogRecorder {
    fn record(&mut self, key: EventKey, event: &Event) -> Result<()> {
        self.fp.add(key, event);
        self.writer.append(key, event)
    }
    fn emit(&mut self, key: EventKey, output: &Output) -> Result<()> {
        self.fp.add_output(key, output);
        self.writer.append_output(key, output)
    }
}

#[derive(Clone, Debug, Default)]
pub struct DriverOptions {
    /// Data before `start` is skipped.
    pub start: Option<UnixNanos>,
    /// Data at or after `end` is not stepped; timers due before `end` still fire.
    pub end: Option<UnixNanos>,
    /// Inputs stepped while the node is `Syncing` (instrument definitions, the account snapshot).
    pub preamble: Vec<Event>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RunSummary {
    pub data_events: u64,
    pub skipped: u64,
    /// Every recorded input, synthesised ones included.
    pub inputs: u64,
    pub outputs: u64,
    pub batches: u64,
    pub timers: u64,
    /// Order events from the simulated venue.
    pub venue_answers: u64,
    /// A strategy error stopped the node (`ErrorPolicy::HaltNode`).
    pub halted: bool,
    /// Orders still open when the node stopped.
    pub left_open: u32,
    pub state: NodeState,
    pub first_ts: UnixNanos,
    pub last_ts: UnixNanos,
}

/// The input stream: plain data, or a venue loop around it.
#[allow(clippy::large_enum_variant)] // one per run
pub enum Source {
    Plain(alloc::boxed::Box<dyn EventSource>),
    Venue(VenueLoop),
}

impl core::fmt::Debug for Source {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Source::Plain(_) => f.write_str("Source::Plain"),
            Source::Venue(v) => f.debug_tuple("Source::Venue").field(v).finish(),
        }
    }
}

#[derive(Debug)]
pub struct Driver<'a, R: Recorder> {
    engine: &'a mut Engine,
    source: Source,
    recorder: &'a mut R,
    options: DriverOptions,
    lifecycle: Lifecycle,
    summary: RunSummary,
    seq: u64,
    last_ts: UnixNanos,
    batch: Option<UnixNanos>,
}

impl<'a, R: Recorder> Driver<'a, R> {
    pub fn new(
        engine: &'a mut Engine,
        source: Source,
        recorder: &'a mut R,
        options: DriverOptions,
    ) -> Self {
        Self {
            engine,
            source,
            recorder,
            options,
            lifecycle: Lifecycle::default(),
            summary: RunSummary::default(),
            seq: 0,
            last_ts: UnixNanos::default(),
            batch: None,
        }
    }

    /// Runs to the end of the data. A failing step ends the run with its error; the summary is
    /// available either way.
    pub fn run(&mut self) -> Result<RunSummary> {
        self.summary = RunSummary::default();
        let result = self.run_to_end();
        self.summary.state = self.lifecycle.state();
        self.summary.last_ts = self.last_ts;
        self.summary.left_open = self.engine.open_orders();
        result.map(|()| self.summary)
    }

    #[must_use]
    pub const fn summary(&self) -> &RunSummary {
        &self.summary
    }
    pub fn source(&mut self) -> &mut Source {
        &mut self.source
    }

    fn halted(&self) -> bool {
        self.engine.halt_requested()
    }

    fn run_to_end(&mut self) -> Result<()> {
        match &self.source {
            Source::Plain(_) => self.run_plain()?,
            Source::Venue(_) => self.run_venue()?,
        }
        self.summary.halted = self.halted();
        if !self.summary.halted {
            if let Some(end) = self.options.end {
                self.fire_timers(end, false)?;
                self.summary.halted = self.halted();
            }
        }
        let final_ts = self.last_ts;
        let reason = if self.summary.halted {
            LifecycleReason::ShutdownRequested
        } else {
            LifecycleReason::EndOfData
        };
        if !is_terminal(self.lifecycle.state()) {
            self.transition(reason, final_ts)?;
            self.transition(LifecycleReason::Drained, final_ts)?;
        }
        self.close_batch()
    }

    fn run_plain(&mut self) -> Result<()> {
        let mut have = self.next_data()?;
        let ts0 = self.options.start.or(have.as_ref().map(|(k, _)| k.ts)).unwrap_or_default();
        self.start(ts0)?;
        while let Some((key, event)) = have.take() {
            if self.halted() {
                break;
            }
            self.fire_timers(key.ts, true)?;
            if self.halted() {
                break;
            }
            self.feed(key, &event)?;
            self.summary.data_events += 1;
            have = self.next_data()?;
        }
        Ok(())
    }

    /// The earliest of venue side, timer and kernel input, in that order on a tie.
    fn run_venue(&mut self) -> Result<()> {
        let venue = self.venue_mut().next_venue_time()?;
        let ts0 = self.options.start.or(venue).unwrap_or_default();
        self.start(ts0)?;
        while !self.halted() {
            let venue = self.venue_mut().next_venue_time()?;
            let input = self.venue_mut().next_input_time();
            if venue.is_none() && input.is_none() {
                break;
            }
            self.advance(venue, input)?;
        }
        Ok(())
    }

    fn venue_mut(&mut self) -> &mut VenueLoop {
        match &mut self.source {
            Source::Venue(v) => v,
            Source::Plain(_) => unreachable!("venue mode"),
        }
    }

    fn advance(&mut self, venue: Option<UnixNanos>, input: Option<UnixNanos>) -> Result<()> {
        let timer = self.engine.next_timer();
        if let Some(v) = venue {
            if timer.is_none_or(|t| t.deadline >= v) && input.is_none_or(|i| i >= v) {
                return self.venue_mut().process_venue();
            }
        }
        if let Some(t) = timer {
            if input.is_none_or(|i| i >= t.deadline) {
                return self.fire_timers(t.deadline, true);
            }
        }
        let Some((key, event)) = self.venue_mut().pop_input() else { return Ok(()) };
        if key.source_id == VENUE_SOURCE {
            self.summary.venue_answers += 1;
        } else {
            self.summary.data_events += 1;
        }
        self.feed(key, &event)
    }

    /// The next data event inside `[start, end)` of a plain source.
    fn next_data(&mut self) -> Result<Option<(EventKey, Event)>> {
        let Source::Plain(source) = &mut self.source else { return Ok(None) };
        loop {
            let Some((key, event)) = source.next()? else { return Ok(None) };
            if self.options.end.is_some_and(|end| key.ts >= end) {
                self.summary.skipped += 1;
                return Ok(None);
            }
            if self.options.start.is_some_and(|start| key.ts < start) {
                self.summary.skipped += 1;
                continue;
            }
            return Ok(Some((key, event)));
        }
    }

    /// `Init -> Running` at `ts0`, with the preamble stepped while `Syncing`.
    fn start(&mut self, ts0: UnixNanos) -> Result<()> {
        self.summary.first_ts = ts0;
        for reason in
            [LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started]
        {
            self.transition(reason, ts0)?;
        }
        if let Source::Venue(v) = &mut self.source {
            for s in 0..self.engine.kernel().config().strategies {
                let id = self.engine.kernel().trading.strategy_id(StrategyIndex(s));
                v.exchange().set_strategy_id(s, id);
            }
        }
        let preamble = core::mem::take(&mut self.options.preamble);
        for e in &preamble {
            // The simulated venue needs the same definitions and balances as the kernel.
            if let Source::Venue(v) = &mut self.source {
                v.exchange().on_data(e, ts0)?;
            }
            self.feed(EventKey::new(ts0, KERNEL_SOURCE, 0), e)?;
        }
        self.options.preamble = preamble;
        self.transition(LifecycleReason::Synced, ts0)
    }

    fn transition(&mut self, reason: LifecycleReason, ts: UnixNanos) -> Result<()> {
        let event = self.lifecycle.apply(reason, ts)?;
        self.feed(EventKey::new(ts, KERNEL_SOURCE, 0), &Event::NodeLifecycle(event))
    }

    /// Fires every timer due at `limit` (inclusive) or before it (exclusive).
    fn fire_timers(&mut self, limit: UnixNanos, inclusive: bool) -> Result<()> {
        while !self.halted() {
            let Some(due) = self.engine.next_timer() else { break };
            if !(due.deadline < limit || (inclusive && due.deadline == limit)) {
                break;
            }
            let ts = if due.deadline < self.last_ts { self.last_ts } else { due.deadline };
            self.feed(
                EventKey::new(ts, KERNEL_SOURCE, 0),
                &Event::TimerFired(TimerFired { key: due.key, deadline: due.deadline }),
            )?;
            self.summary.timers += 1;
        }
        Ok(())
    }

    /// Steps one input, closing the open batch first when the input starts a new one.
    fn feed(&mut self, key: EventKey, event: &Event) -> Result<()> {
        if self.batch.is_some_and(|ts| ts != key.ts) {
            self.close_batch()?;
        }
        self.batch = Some(key.ts);
        self.step_input(key, event)
    }

    fn close_batch(&mut self) -> Result<()> {
        let Some(ts) = self.batch.take() else { return Ok(()) };
        self.summary.batches += 1;
        self.step_input(EventKey::new(ts, KERNEL_SOURCE, 0), &Event::BatchEnd(BatchEnd { ts }))
    }

    fn step_input(&mut self, mut key: EventKey, event: &Event) -> Result<()> {
        self.seq += 1;
        key.seq = self.seq;
        self.recorder.record(key, event)?;
        self.summary.inputs += 1;
        self.last_ts = key.ts;
        self.engine.step(key, event)?;
        if let Source::Venue(v) = &mut self.source {
            v.on_outputs(key.ts, self.engine.outputs())?;
        }
        for (i, o) in self.engine.outputs().iter().enumerate() {
            self.recorder.emit(EventKey::new(key.ts, i as u16, key.seq), o)?;
        }
        self.summary.outputs += self.engine.outputs().len() as u64;
        Ok(())
    }
}
