//! The backtest's two timelines (docs/architecture.md section 12.2). The simulated venue acts at
//! venue time; the kernel observes later:
//!
//! - market data: the venue matches on it at its recorded `ts`; the kernel sees it `L_feed` later
//!   (its `ts_init` is rewritten to that time);
//! - commands: a command the kernel emits at `t` reaches the venue at `t + L_out`;
//! - answers: an order event the venue emits at `t` reaches the kernel at `t + L_in`.
//!
//! Each direction is one ordered channel (the feed, the WS API connection, the user data stream),
//! so every channel is a FIFO: a delay never lets a later message overtake an earlier one on the
//! same channel. Delays come from `JitteredLatency`, keyed by the data counter, the `ClientOrderId`
//! and the answer counter: a function of the inputs, like everything else.
//!
//! The driver asks for the next venue-side time and the next kernel input time and processes the
//! earliest (venue side first on a tie).

use alloc::boxed::Box;

use cost::{JitteredLatency, LatencyHop};
use kernel_core::{DurationNanos, EventKey, FixedVec, Result, Status, UnixNanos};
use model::order_events::OrderEvent;
use model::outputs::Output;
use model::Event;

use crate::sim::{SimConfig, SimulatedExchange};
use crate::source::EventSource;

/// `source_id` of the venue's answers in the run log.
pub const VENUE_SOURCE: u16 = 0xFFFE;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VenueLoopConfig {
    pub sim: SimConfig,
    pub feed: DurationNanos,
    pub outbound: DurationNanos,
    pub inbound: DurationNanos,
    pub jitter: DurationNanos,
    pub seed: u64,
    /// Kernel inputs in flight per channel.
    pub pending: u32,
    /// Commands in flight.
    pub commands: u32,
    pub start: Option<UnixNanos>,
    pub end: Option<UnixNanos>,
}

impl Default for VenueLoopConfig {
    fn default() -> Self {
        Self {
            sim: SimConfig::default(),
            feed: DurationNanos::default(),
            outbound: DurationNanos::default(),
            inbound: DurationNanos::default(),
            jitter: DurationNanos::default(),
            seed: 0,
            pending: 4096,
            commands: 4096,
            start: None,
            end: None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VenueLoopStats {
    /// Market data events the venue saw.
    pub data: u64,
    /// Order events the venue sent.
    pub answers: u64,
    /// Commands delivered to the venue.
    pub commands: u64,
    /// Data outside `[start, end)`.
    pub skipped: u64,
}

/// A fixed-capacity FIFO.
#[derive(Debug)]
struct Ring<T> {
    items: FixedVec<Option<T>>,
    head: usize,
    count: usize,
}

impl<T> Ring<T> {
    fn new(capacity: usize) -> Self {
        Self { items: FixedVec::from_iter_exact((0..capacity).map(|_| None)), head: 0, count: 0 }
    }
    const fn is_empty(&self) -> bool {
        self.count == 0
    }
    fn front(&self) -> Option<&T> {
        if self.count == 0 {
            return None;
        }
        self.items[self.head].as_ref()
    }
    fn push(&mut self, item: T) -> Result<()> {
        if self.count == self.items.len() {
            return Err(Status::CapacityExceeded);
        }
        let at = (self.head + self.count) % self.items.len();
        self.items[at] = Some(item);
        self.count += 1;
        Ok(())
    }
    fn pop(&mut self) -> Option<T> {
        if self.count == 0 {
            return None;
        }
        let item = self.items[self.head].take();
        self.head = (self.head + 1) % self.items.len();
        self.count -= 1;
        item
    }
}

#[derive(Debug)]
struct Delayed {
    key: EventKey,
    event: Event,
}

#[derive(Debug)]
struct Command {
    at: UnixNanos,
    output: Output,
}

/// Sets an event's `ts_init` (the time the kernel observes it).
fn set_ts_init(event: &mut Event, ts: UnixNanos) {
    match event {
        Event::TradeTick(e) => e.ts_init = ts,
        Event::QuoteTick(e) => e.ts_init = ts,
        Event::OrderBookDeltas(e) => {
            e.ts_init = ts;
            for d in &mut e.deltas {
                d.ts_init = ts;
            }
        }
        Event::Bar(e) => e.ts_init = ts,
        Event::MarkPriceUpdate(e) => e.ts_init = ts,
        Event::IndexPriceUpdate(e) => e.ts_init = ts,
        Event::FundingRateUpdate(e) => e.ts_init = ts,
        Event::InstrumentStatus(e) => e.ts_init = ts,
        Event::InstrumentClose(e) => e.ts_init = ts,
        Event::LiquidationOrder(e) => e.ts_init = ts,
        Event::Instrument(e) => e.ts_init = ts,
        Event::AccountState(e) => e.ts_init = ts,
        Event::RateLimitFeedback(e) => e.ts_init = ts,
        Event::Order(e) => set_order_ts_init(e, ts),
        Event::TimerFired(_)
        | Event::BatchEnd(_)
        | Event::NodeLifecycle(_)
        | Event::Shutdown(_) => {}
    }
}

fn set_order_ts_init(e: &mut OrderEvent, ts: UnixNanos) {
    match e {
        OrderEvent::Initialized(x) => x.header.ts_init = ts,
        OrderEvent::Denied(x) => x.header.ts_init = ts,
        OrderEvent::Emulated(x) => x.header.ts_init = ts,
        OrderEvent::Released(x) => x.header.ts_init = ts,
        OrderEvent::Submitted(x) => x.header.ts_init = ts,
        OrderEvent::Accepted(x) => x.header.ts_init = ts,
        OrderEvent::Rejected(x) => x.header.ts_init = ts,
        OrderEvent::Canceled(x) => x.header.ts_init = ts,
        OrderEvent::Expired(x) => x.header.ts_init = ts,
        OrderEvent::Triggered(x) => x.header.ts_init = ts,
        OrderEvent::PendingUpdate(x) => x.header.ts_init = ts,
        OrderEvent::PendingCancel(x) => x.header.ts_init = ts,
        OrderEvent::ModifyRejected(x) => x.header.ts_init = ts,
        OrderEvent::CancelRejected(x) => x.header.ts_init = ts,
        OrderEvent::Updated(x) => x.header.ts_init = ts,
        OrderEvent::Filled(x) => x.header.ts_init = ts,
        OrderEvent::FillVoided(x) => x.header.ts_init = ts,
    }
}

fn fnv1a(text: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in text.bytes() {
        h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
    }
    h
}

/// A FIFO channel's delivery time: never before the previous message's.
fn fifo(last: &mut UnixNanos, t: UnixNanos) -> UnixNanos {
    let t = if t < *last { *last } else { t };
    *last = t;
    t
}

pub struct VenueLoop {
    config: VenueLoopConfig,
    source: Box<dyn EventSource>,
    exchange: SimulatedExchange,
    latency: JitteredLatency,
    data: Ring<Delayed>,
    answers: Ring<Delayed>,
    commands: Ring<Command>,
    stats: VenueLoopStats,
    lookahead: Option<(EventKey, Event)>,
    source_done: bool,
    last_data: UnixNanos,
    last_answer: UnixNanos,
    last_command: UnixNanos,
    data_serial: u64,
    answer_serial: u64,
    command_serial: u32,
}

impl core::fmt::Debug for VenueLoop {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("VenueLoop").field("stats", &self.stats).finish_non_exhaustive()
    }
}

impl VenueLoop {
    #[must_use]
    pub fn new(config: VenueLoopConfig, source: Box<dyn EventSource>) -> Self {
        let z = DurationNanos::default();
        Self {
            exchange: SimulatedExchange::new(config.sim),
            latency: JitteredLatency::new(
                config.seed,
                [config.feed, config.outbound, config.inbound],
                [z, config.jitter, config.jitter],
            ),
            data: Ring::new(config.pending as usize),
            answers: Ring::new(config.pending as usize),
            commands: Ring::new(config.commands as usize),
            stats: VenueLoopStats::default(),
            lookahead: None,
            source_done: false,
            last_data: UnixNanos::default(),
            last_answer: UnixNanos::default(),
            last_command: UnixNanos::default(),
            data_serial: 0,
            answer_serial: 0,
            command_serial: 0,
            config,
            source,
        }
    }

    pub fn exchange(&mut self) -> &mut SimulatedExchange {
        &mut self.exchange
    }
    #[must_use]
    pub const fn stats(&self) -> &VenueLoopStats {
        &self.stats
    }

    /// The earliest venue-side time: the next market data event or command arrival.
    pub fn next_venue_time(&mut self) -> Result<Option<UnixNanos>> {
        self.fill_lookahead()?;
        let mut out = self.lookahead.as_ref().map(|(k, _)| k.ts);
        if let Some(c) = self.commands.front() {
            if out.is_none_or(|t| c.at < t) {
                out = Some(c.at);
            }
        }
        Ok(out)
    }

    /// The earliest kernel input: answers before market data at the same time (the user data
    /// stream drains first, section 5.5).
    #[must_use]
    pub fn next_input_time(&self) -> Option<UnixNanos> {
        let mut t = self.answers.front().map(|d| d.key.ts);
        if let Some(d) = self.data.front() {
            if t.is_none_or(|t| d.key.ts < t) {
                t = Some(d.key.ts);
            }
        }
        t
    }

    /// Processes the earliest venue-side item (market data first on a tie).
    pub fn process_venue(&mut self) -> Result<()> {
        self.fill_lookahead()?;
        let take_data = match (&self.lookahead, self.commands.front()) {
            (Some(_), None) => true,
            (Some((k, _)), Some(c)) => c.at >= k.ts,
            (None, _) => false,
        };
        if take_data {
            return self.process_data();
        }
        let Some(c) = self.commands.pop() else { return Ok(()) };
        self.stats.commands += 1;
        self.exchange.clear_events();
        self.exchange.on_command(&c.output, c.at)?;
        self.schedule_answers(c.at)
    }

    /// Pops the earliest kernel input.
    pub fn pop_input(&mut self) -> Option<(EventKey, Event)> {
        let answer = match (self.answers.front(), self.data.front()) {
            (Some(_), None) => true,
            (Some(a), Some(d)) => d.key.ts >= a.key.ts,
            (None, _) => false,
        };
        let d = if answer { self.answers.pop()? } else { self.data.pop()? };
        Some((d.key, d.event))
    }

    /// Schedules the venue commands a step at kernel time `now` emitted.
    pub fn on_outputs(&mut self, now: UnixNanos, outputs: &[Output]) -> Result<()> {
        for o in outputs {
            if !o.is_venue_command() {
                continue; // features, records, denials: not for the venue
            }
            let identity = o.client_order_id().map_or(0, |cid| fnv1a(cid.as_str()));
            self.command_serial += 1;
            let delay = self.latency.delay(identity, LatencyHop::Outbound, self.command_serial);
            let at = fifo(&mut self.last_command, now.plus(delay)?);
            self.commands.push(Command { at, output: *o })?;
        }
        Ok(())
    }

    #[must_use]
    pub fn exhausted(&self) -> bool {
        self.source_done
            && self.lookahead.is_none()
            && self.commands.is_empty()
            && self.data.is_empty()
            && self.answers.is_empty()
    }

    fn fill_lookahead(&mut self) -> Result<()> {
        while self.lookahead.is_none() && !self.source_done {
            let Some((key, event)) = self.source.next()? else {
                self.source_done = true;
                return Ok(());
            };
            if self.config.end.is_some_and(|end| key.ts >= end) {
                self.stats.skipped += 1;
                self.source_done = true;
                return Ok(());
            }
            if self.config.start.is_some_and(|start| key.ts < start) {
                self.stats.skipped += 1;
                continue;
            }
            self.lookahead = Some((key, event));
        }
        Ok(())
    }

    fn process_data(&mut self) -> Result<()> {
        let Some((key, mut event)) = self.lookahead.take() else { return Ok(()) };
        let venue_time = key.ts;
        self.stats.data += 1;
        self.exchange.clear_events();
        self.exchange.on_data(&event, venue_time)?;
        self.schedule_answers(venue_time)?;
        self.data_serial += 1;
        let delay = self.latency.delay(self.data_serial, LatencyHop::Feed, 0);
        let ts = fifo(&mut self.last_data, venue_time.plus(delay)?);
        set_ts_init(&mut event, ts);
        self.data.push(Delayed { key: EventKey::new(ts, key.source_id, key.seq), event })
    }

    fn schedule_answers(&mut self, venue_time: UnixNanos) -> Result<()> {
        for i in 0..self.exchange.events().len() {
            let e = self.exchange.events()[i];
            self.answer_serial += 1;
            let delay = self.latency.delay(self.answer_serial, LatencyHop::Inbound, 0);
            let at = fifo(&mut self.last_answer, venue_time.plus(delay)?);
            let mut event = Event::Order(e);
            set_ts_init(&mut event, at);
            self.answers.push(Delayed { key: EventKey::new(at, VENUE_SOURCE, 0), event })?;
            self.stats.answers += 1;
        }
        self.exchange.clear_events();
        Ok(())
    }
}
