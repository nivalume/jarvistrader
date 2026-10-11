//! Replay (docs/architecture.md sections 7.7 and 16.4): step the inputs of a run log into a fresh
//! engine, recompute every output and compare it with the recorded one. The first difference is a
//! [`Divergence`] at that input's seq.
//!
//! `EngineState` snapshots (section 16.3): the replay takes a snapshot at the same points as the
//! node (`engine::snapshot_schedule`, `persistence.snapshot_every`) and compares it with the
//! snapshot file of that seq when the run directory has one; a different state is a divergence.
//! With `from_snapshot` the replay starts from that snapshot file instead of the first input. A
//! log that does not start at seq 1 (one whose older segments were truncated) starts from the
//! earliest snapshot of its directory that covers its first record.

use std::path::{Path, PathBuf};

use engine::lifecycle::Lifecycle;
use engine::snapshot_schedule::{asks_for_snapshot, SnapshotSchedule};
use engine::Engine;
use kernel_core::sha256::{hex, Sha256};
use kernel_core::{EventKey, Status};
use model::enums::StrategyErrorKind;
use model::log::{Record, RecordBody};
use model::outputs::Output;
use model::Event;
use strategy::{Strategy, StrategyFailure};

use crate::backtest_node::{error_policy, kernel_config, name_strategies};
use crate::config::NodeConfig;
use crate::event_log::{EventLogReadOptions, EventLogReader};
use crate::snapshot_file::{
    decode_snapshot, load_snapshot, pick_snapshot, snapshot_name, SnapshotPick,
};
use crate::{NodeError, NodeResult};

#[derive(Clone, Debug, Default)]
pub struct ReplayOptions {
    /// Stop after the input with this seq and its outputs.
    pub until: Option<u64>,
    /// Report the SHA-256 of the engine's state after the last step.
    pub dump_state: bool,
    pub check_snapshots: bool,
    /// A snapshot file of this run to start from.
    pub from_snapshot: Option<PathBuf>,
    /// Accept the end a crash leaves: a torn last record, and missing outputs of the last input.
    pub tolerate_torn_end: bool,
}

impl ReplayOptions {
    #[must_use]
    pub fn new() -> Self {
        Self { check_snapshots: true, ..Self::default() }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Divergence {
    pub seq: u64,
    /// What the log holds at that point.
    pub recorded: String,
    /// What the replay computed instead.
    pub replayed: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReplayReport {
    pub inputs: u64,
    pub outputs: u64,
    pub divergence: Option<Divergence>,
    /// `dump_state`: `state: at seq=<n> sha256 <hex> bytes <n>; ...` of the engine state after
    /// the last step.
    pub state: String,
    /// Snapshot files that matched the replayed state.
    pub snapshots_checked: u64,
    /// The seq of the snapshot the replay started from, if any.
    pub start_seq: u64,
    /// The last input stepped (or the snapshot's seq).
    pub last_seq: u64,
    pub torn_bytes: u64,
    /// `tolerate_torn_end`: the last input's outputs were missing.
    pub torn_step: bool,
}

fn output_line(key: EventKey, output: &Output) -> String {
    format!("output {} of seq {}: {output:?}", key.source_id, key.seq)
}

fn input_line(key: EventKey, event: &Event) -> String {
    format!(
        "input seq {} ts {} source {}: {:?}",
        key.seq,
        key.ts.value(),
        key.source_id,
        event.kind()
    )
}

struct Replayer<'a> {
    engine: &'a mut Engine,
    lifecycle: Lifecycle,
    options: &'a ReplayOptions,
    report: ReplayReport,
    directory: PathBuf,
    snapshots: SnapshotSchedule,
    skip_until: u64,
    computed: Vec<(EventKey, Output)>,
    matched: usize,
    /// Failures the last step reproduced, which the next inputs must record as `StrategyError`.
    expected_errors: Vec<StrategyFailure>,
    last_seq: u64,
    restored: bool,
    stopped: bool,
}

fn failure_text(f: &StrategyFailure) -> String {
    format!(
        "StrategyError strategy_index={} kind={:?} message_hash={}",
        f.strategy.0, f.kind, f.message_hash
    )
}

impl Replayer<'_> {
    fn diverge(&mut self, seq: u64, recorded: String, replayed: String) {
        if self.report.divergence.is_none() {
            self.report.divergence = Some(Divergence { seq, recorded, replayed });
        }
    }

    fn next_computed(&self) -> String {
        let (key, output) = &self.computed[self.matched];
        output_line(*key, output)
    }

    /// Restores the engine from a snapshot file of this run; the replay starts after its seq.
    fn restore(&mut self, path: &Path) -> NodeResult<()> {
        let snapshot = load_snapshot(path)?;
        self.engine.load_state(snapshot.body()).map_err(|s| {
            NodeError::new(s, format!("{}: cannot restore the snapshot: {s}", path.display()))
        })?;
        self.lifecycle = Lifecycle::at(self.engine.kernel().node_state);
        self.snapshots.resume_after(snapshot.info.seq);
        self.skip_until = snapshot.info.seq;
        self.last_seq = snapshot.info.seq;
        self.restored = true;
        self.report.start_seq = snapshot.info.seq;
        self.report.last_seq = snapshot.info.seq;
        Ok(())
    }

    /// A log that starts after seq 1 needs a snapshot at or after the state before its first
    /// record: the seq before it when that is an input, its own seq when it is an output.
    fn start_point(&mut self, record: &Record) -> NodeResult<()> {
        let output = matches!(record.body, RecordBody::Output(_));
        let seq = record.key.seq;
        let needed = if output { seq } else { seq.saturating_sub(1) };
        if self.restored {
            if self.skip_until < needed {
                return Err(NodeError::new(
                    Status::InvalidArgument,
                    format!(
                        "the snapshot at seq {} is older than the log, which starts at seq {seq}",
                        self.skip_until
                    ),
                ));
            }
            return Ok(());
        }
        if needed == 0 {
            return Ok(());
        }
        let Some(snapshot) = pick_snapshot(&self.directory, needed, SnapshotPick::Earliest) else {
            return Err(NodeError::new(
                Status::NotFound,
                format!(
                    "the log starts at seq {seq} (it continues an earlier run, or its older \
                     segments were removed) and no complete snapshot of this build in the \
                     directory covers it"
                ),
            ));
        };
        self.restore(&snapshot.path)
    }

    fn on_output(&mut self, key: EventKey, logged: &Output) {
        if self.matched >= self.computed.len() {
            self.diverge(key.seq, output_line(key, logged), "no output".to_string());
        } else if self.computed[self.matched].0 != key || self.computed[self.matched].1 != *logged {
            let computed = self.next_computed();
            self.diverge(key.seq, output_line(key, logged), computed);
        } else {
            self.matched += 1;
            self.report.outputs += 1;
        }
    }

    fn on_input(&mut self, key: EventKey, event: &Event) {
        if self.matched < self.computed.len() {
            let computed = self.next_computed();
            self.diverge(self.last_seq, "no output".to_string(), computed);
            return;
        }
        if self.options.until.is_some_and(|until| key.seq > until) {
            self.stopped = true;
            return;
        }
        // Recorded failures are read from the log, not re-raised (section 7.6): each must match
        // one the replay reproduced in the step before, in order; an Overrun comes from a wall
        // clock measurement and is taken as recorded.
        if let Event::StrategyError(e) = event {
            if e.kind != StrategyErrorKind::Overrun {
                let expected = self.expected_errors.first().copied();
                match expected {
                    None => {
                        self.diverge(
                            key.seq,
                            input_line(key, event),
                            "no strategy failure at this point".to_string(),
                        );
                        return;
                    }
                    Some(f)
                        if f.strategy.0 != e.strategy_index || f.message_hash != e.message_hash =>
                    {
                        self.diverge(key.seq, input_line(key, event), failure_text(&f));
                        return;
                    }
                    Some(_) => {
                        self.expected_errors.remove(0);
                    }
                }
            }
        } else if let Some(f) = self.expected_errors.first().copied() {
            self.diverge(key.seq, input_line(key, event), failure_text(&f));
            return;
        }
        if let Event::NodeLifecycle(lc) = event {
            let from = self.lifecycle.state();
            if self.lifecycle.apply(lc.reason, lc.ts).is_err() {
                self.diverge(
                    key.seq,
                    input_line(key, event),
                    format!("a transition the lifecycle does not allow from {from:?}"),
                );
                return;
            }
        }
        if let Err(s) = self.engine.step(key, event) {
            self.diverge(key.seq, input_line(key, event), format!("the step failed: {s}"));
            return;
        }
        self.computed.clear();
        self.matched = 0;
        for (i, o) in self.engine.outputs().iter().enumerate() {
            self.computed.push((EventKey::new(key.ts, i as u16, key.seq), *o));
        }
        if !matches!(event, Event::StrategyError(_)) {
            self.expected_errors.extend_from_slice(self.engine.failures());
        }
        self.last_seq = key.seq;
        self.report.inputs += 1;
        if asks_for_snapshot(event, self.engine.stop_requested()) {
            self.snapshots.request();
        }
        if self.snapshots.due(key.seq, matches!(event, Event::BatchEnd(_))) {
            self.check_snapshot(key.seq);
        }
    }

    /// The state here against the node's snapshot file of this seq, if the directory has it.
    fn check_snapshot(&mut self, seq: u64) {
        if !self.options.check_snapshots {
            return;
        }
        let name = snapshot_name(seq);
        let Ok(file) = sys::read_bytes(&self.directory.join(&name)) else { return };
        let Ok((_, _, body)) = decode_snapshot(&file) else {
            self.diverge(
                seq,
                format!("an unreadable snapshot file {name}"),
                "a snapshot".to_string(),
            );
            return;
        };
        let Ok(state) = self.engine.save_state() else {
            self.diverge(
                seq,
                "a snapshot".to_string(),
                "no snapshot: saving the state failed".to_string(),
            );
            return;
        };
        let body = &file[body];
        if state != body {
            let at = state.iter().zip(body).take_while(|(a, b)| a == b).count();
            self.diverge(
                seq,
                format!("snapshot {name} ({} bytes)", body.len()),
                format!("a state differing from byte {at} ({} bytes)", state.len()),
            );
            return;
        }
        self.report.snapshots_checked += 1;
    }

    /// End of the log: every computed output must have been recorded, unless the log may end
    /// where a crash cut it.
    fn finish(&mut self) {
        if self.report.divergence.is_some() {
            return;
        }
        if self.options.tolerate_torn_end {
            self.report.torn_step =
                self.matched < self.computed.len() || !self.expected_errors.is_empty();
            return;
        }
        if self.matched < self.computed.len() {
            let computed = self.next_computed();
            self.diverge(self.last_seq, "no output".to_string(), computed);
        } else if let Some(f) = self.expected_errors.first() {
            self.diverge(self.last_seq, "the end of the log".to_string(), failure_text(f));
        }
    }

    fn run(&mut self, reader: &mut EventLogReader) -> NodeResult<()> {
        let mut first = true;
        while self.report.divergence.is_none() && !self.stopped {
            let record = reader.next_record().map_err(|s| {
                NodeError::new(s, format!("unreadable record after seq {}: {s}", self.last_seq))
            })?;
            let Some(record) = record else {
                self.finish();
                break;
            };
            if first {
                first = false;
                self.start_point(&record)?;
            }
            if record.key.seq <= self.skip_until {
                continue;
            }
            match &record.body {
                RecordBody::Output(o) => self.on_output(record.key, o),
                RecordBody::Input(e) => self.on_input(record.key, e),
            }
        }
        self.report.last_seq = self.last_seq;
        if self.options.dump_state {
            let state = self.engine.save_state().unwrap_or_default();
            let digest = hex(&Sha256::digest(&state));
            let kernel = self.engine.kernel();
            self.report.state = format!(
                "state: at seq={} sha256 {} bytes {}; instruments {}, features {}, strategies {}\n",
                self.last_seq,
                String::from_utf8_lossy(&digest),
                state.len(),
                kernel.instruments.len(),
                kernel.features.len(),
                self.engine.strategy_count()
            );
        }
        Ok(())
    }
}

/// Replays the run log in `directory` into `engine`, built from the run's configuration and not
/// yet stepped.
pub fn replay_into(
    directory: &Path,
    config: &NodeConfig,
    engine: &mut Engine,
    options: &ReplayOptions,
) -> NodeResult<ReplayReport> {
    let shown = directory.display();
    let mut reader = EventLogReader::open(
        directory,
        EventLogReadOptions { tolerate_torn_tail: options.tolerate_torn_end },
    )
    .map_err(|s| NodeError::new(s, format!("{shown}: cannot open the run log: {s}")))?;
    let mut replayer = Replayer {
        engine,
        lifecycle: Lifecycle::default(),
        options,
        report: ReplayReport::default(),
        directory: directory.to_path_buf(),
        snapshots: SnapshotSchedule::new(config.persistence.snapshot_every),
        skip_until: 0,
        computed: Vec::new(),
        matched: 0,
        expected_errors: Vec::new(),
        last_seq: 0,
        restored: false,
        stopped: false,
    };
    if let Some(path) = &options.from_snapshot {
        replayer.restore(path)?;
    }
    replayer
        .run(&mut reader)
        .map_err(|e| NodeError::new(e.status, format!("{shown}: {}", e.message)))?;
    let mut report = replayer.report;
    report.torn_bytes = reader.torn_bytes();
    Ok(report)
}

/// Replays the run in `directory` with fresh `strategies`, built from `config`.
pub fn replay_run(
    directory: &Path,
    config: &NodeConfig,
    strategies: Vec<Box<dyn Strategy>>,
    options: &ReplayOptions,
) -> NodeResult<ReplayReport> {
    let mut engine = Engine::new(kernel_config(config), strategies, error_policy(config))
        .map_err(|s| NodeError::new(s, "cannot build the engine"))?;
    name_strategies(config, engine.kernel_mut());
    replay_into(directory, config, &mut engine, options)
}
