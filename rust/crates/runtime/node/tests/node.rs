//! The node end to end: the golden catalog written to disk, a backtest of the probe strategy
//! against the simulated venue recorded to a run directory with snapshots, the run replayed
//! (whole and from a snapshot), a second run identical byte for byte, and the report.

use std::path::{Path, PathBuf};

use corpus::golden;
use model::enums::NodeState;
use node::event_log::{EventLogOptions, EventLogReadOptions, EventLogReader, EventLogWriter};
use node::snapshot_file::{self, SnapshotInfo};
use node::{
    build_run_report, parse_config, replay_run, report_text, run_backtest, BacktestRequest,
    ConfigOverrides, Registry, ReplayOptions, RunManifest,
};

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("jarvis-node-tests-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn write_catalog(dir: &Path, seed: u64) {
    let id = golden::instrument_id();
    for stream in golden::catalog(seed).unwrap() {
        node::catalog::write_stream(dir, &id, stream.name, golden::DAY_NAME, &stream.records)
            .unwrap();
    }
    let instrument = golden::instrument_stream().unwrap();
    node::catalog::write_stream(dir, &id, instrument.name, golden::DAY_NAME, &instrument.records)
        .unwrap();
}

const CONFIG: &str = r#"
[node]
id = "golden"
seed = 3

[data]
catalog = "catalog"
range = { start = "2026-09-01T00:00:00Z", end = "2026-09-01T00:03:00Z" }

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["BTCUSDT-PERP.BINANCE"]
streams = ["aggTrade", "bookTicker"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"

[venues.sim]
fill_model = "queue_position"
latency = { feed_ns = 1000000, out_ns = 20000000, in_ns = 5000000, jitter_ns = 2000000 }
fee = { schedule = "binance_usdm_vip0" }
balances = ["10000 USDT"]

[[strategies]]
id = "golden-001"
impl = "rust:Probe"
params = { plan = "orders" }
"#;

fn overrides(catalog: &Path, extra: &[&str]) -> ConfigOverrides {
    let mut sets = vec![format!("data.catalog={}", catalog.display())];
    sets.extend(extra.iter().map(|s| (*s).to_string()));
    ConfigOverrides { env: None, sets }
}

fn run(catalog: &Path, out: &Path, extra: &[&str]) -> (node::NodeConfig, node::BacktestResult) {
    let overrides = overrides(catalog, extra);
    let config = parse_config(CONFIG, &overrides)
        .unwrap_or_else(|e| panic!("{}", node::format_errors("node.toml", &e)));
    let strategies = Registry::with_builtins().create_all(&config.strategies).unwrap();
    let request = BacktestRequest {
        manifest: RunManifest {
            config_text: CONFIG.to_string(),
            source: "node.toml".to_string(),
            overrides,
            resumed_from: String::new(),
        },
        out: Some(out.to_path_buf()),
        ..BacktestRequest::default()
    };
    let result = run_backtest(&config, &request, strategies).unwrap_or_else(|e| panic!("{e}"));
    (config, result)
}

fn log_bytes(dir: &Path) -> Vec<u8> {
    let mut names: Vec<PathBuf> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|e| e == "jlog"))
        .collect();
    names.sort();
    names.iter().flat_map(|p| std::fs::read(p).unwrap()).collect()
}

#[test]
fn orders_plan_runs_replays_and_reports() {
    let root = scratch("orders");
    let catalog = root.join("catalog");
    write_catalog(&catalog, 7);
    let out = root.join("run");
    let (config, result) = run(&catalog, &out, &["persistence.snapshot_every=100"]);
    let s = result.summary;
    assert_eq!(result.directory.as_deref(), Some(out.as_path()));
    assert_eq!(s.state, NodeState::Stopped);
    assert!(!s.halted);
    assert!(s.data_events > 100, "data events: {}", s.data_events);
    assert!(s.venue_answers > 20, "venue answers: {}", s.venue_answers);
    assert!(s.outputs > 50, "outputs: {}", s.outputs);
    assert!(s.snapshots >= 5, "snapshots: {}", s.snapshots);
    assert_eq!(s.snapshot_failures, 0);
    assert!(out.join("config.toml").is_file());
    assert!(out.join("run.toml").is_file());
    assert!(out.join("events-000000.jlog").is_file());
    let run_toml = std::fs::read_to_string(out.join("run.toml")).unwrap();
    assert!(run_toml.contains("source = \"node.toml\""));
    assert!(run_toml.contains(&format!("config_hash = \"{}\"", node::config_hash_hex(&config))));
    assert_eq!(snapshot_file::list_snapshots(&out).len() as u64, s.snapshots);

    // The run directory alone rebuilds the configuration, and the replay reproduces the log.
    let loaded = node::load_run_config(&out).unwrap();
    assert_eq!(loaded, config);
    let strategies = Registry::with_builtins().create_all(&loaded.strategies).unwrap();
    let report = replay_run(&out, &loaded, strategies, &ReplayOptions::new()).unwrap();
    assert_eq!(report.divergence, None);
    assert_eq!(report.inputs, s.inputs);
    assert_eq!(report.outputs, s.outputs);
    assert_eq!(report.snapshots_checked, s.snapshots);

    // From a snapshot in the middle: the rest of the log replays.
    let snapshots = snapshot_file::list_snapshots(&out);
    let middle = &snapshots[snapshots.len() / 2];
    let strategies = Registry::with_builtins().create_all(&loaded.strategies).unwrap();
    let options =
        ReplayOptions { from_snapshot: Some(middle.path.clone()), ..ReplayOptions::new() };
    let partial = replay_run(&out, &loaded, strategies, &options).unwrap();
    assert_eq!(partial.divergence, None);
    assert_eq!(partial.start_seq, middle.seq);
    assert!(partial.inputs < report.inputs);
    assert_eq!(partial.last_seq, report.last_seq);
    assert_eq!(partial.snapshots_checked, s.snapshots - (snapshots.len() / 2 + 1) as u64);

    // The same run again is the same log, byte for byte.
    let again = root.join("again");
    let (_, result2) = run(&catalog, &again, &["persistence.snapshot_every=100"]);
    assert_eq!(result2.summary, s);
    assert_eq!(log_bytes(&out), log_bytes(&again));
    for entry in &snapshots {
        let other = again.join(entry.path.file_name().unwrap());
        assert_eq!(std::fs::read(&entry.path).unwrap(), std::fs::read(other).unwrap());
    }

    // A strategy with other parameters diverges at the first output it changes.
    let mut other = loaded.clone();
    other.strategies[0].params[0].value =
        node::config::ParamValue::Scalar(node::config::ParamScalar::Str("quote".to_string()));
    let strategies = Registry::with_builtins().create_all(&other.strategies).unwrap();
    let diverged = replay_run(&out, &loaded, strategies, &ReplayOptions::new()).unwrap();
    let d = diverged.divergence.expect("a divergence");
    assert!(d.seq > 0);
    assert_ne!(d.recorded, d.replayed);

    // The report, from the run directory alone.
    let report = build_run_report(&out).unwrap();
    let text = report_text(&report);
    assert!(text.contains("node: golden (backtest, seed 3)"), "{text}");
    assert!(text.contains("strategy golden-001 on BTCUSDT-PERP.BINANCE:"), "{text}");
    assert!(text.contains("account: 10000.00000000 USDT -> "), "{text}");
    let (_, counts) = report.orders.iter().find(|(s, _)| s == "golden-001").expect("counts");
    assert!(counts.submitted > 10, "{text}");
    assert!(counts.accepted > 0 && counts.filled > 0 && counts.canceled > 0, "{text}");
    assert!(counts.denied > 0 && counts.rejected > 0 && counts.expired > 0, "{text}");
    assert!(counts.modifies > 0, "{text}");
    assert_eq!(report.rows.len(), 1);
    assert!(report.rows[0].fills > 0);

    // A second run into a directory that already holds a log is refused.
    let request = BacktestRequest { out: Some(out.clone()), ..BacktestRequest::default() };
    let err = run_backtest(&config, &request, Vec::new()).unwrap_err();
    assert_eq!(err.status, kernel_core::Status::AlreadyExists);
}

#[test]
fn every_probe_plan_replays() {
    let root = scratch("plans");
    let catalog = root.join("catalog");
    write_catalog(&catalog, 7);
    for plan in ["trade", "batch", "quote", "book", "bar", "feature"] {
        let out = root.join(plan);
        let sets = [
            format!("strategies.golden-001.params.plan={plan}"),
            "data.streams.0.streams=[\"aggTrade\", \"bookTicker\", \"depth\", \"kline_1m\"]"
                .to_string(),
            "data.range.end=2026-09-01T00:10:00Z".to_string(),
        ];
        let sets: Vec<&str> = sets.iter().map(String::as_str).collect();
        let (config, result) = run(&catalog, &out, &sets);
        let s = result.summary;
        assert_eq!(s.state, NodeState::Stopped, "{plan}");
        assert!(s.outputs > 0, "{plan}: no outputs");
        let strategies = Registry::with_builtins().create_all(&config.strategies).unwrap();
        let report = replay_run(&out, &config, strategies, &ReplayOptions::new()).unwrap();
        assert_eq!(report.divergence, None, "{plan}");
        assert_eq!(report.outputs, s.outputs, "{plan}");
    }
}

#[test]
fn event_log_rolls_segments_and_reads_them_back() {
    let dir = scratch("segments").join("log");
    let header = node::catalog::catalog_header("test").unwrap();
    let options = EventLogOptions { segment_bytes: 4096, ..EventLogOptions::default() };
    let mut writer = EventLogWriter::open(&dir, &header, options).unwrap();
    let stream = golden::catalog(1).unwrap();
    let quotes = &stream[1].records;
    for (key, event) in quotes {
        writer.append(*key, event).unwrap();
    }
    writer.close().unwrap();
    assert!(writer.segment_index() > 2, "segments: {}", writer.segment_index() + 1);
    assert!(dir.join("events-000001.jlog").is_file());
    let reader = EventLogReader::open(&dir, EventLogReadOptions::default()).unwrap();
    assert_eq!(reader.header(), &header);
    let read: Vec<_> = reader.map(|r| r.unwrap()).collect();
    assert_eq!(read.len(), quotes.len());
    for (record, (key, event)) in read.iter().zip(quotes) {
        assert_eq!(record.key, *key);
        assert_eq!(record.input(), Some(event));
    }
    assert!(EventLogWriter::open(&dir, &header, options).is_err());

    // A torn tail ends the log with the option, fails it without.
    let last = dir.join(node::event_log::segment_name(writer.segment_index()));
    let mut bytes = std::fs::read(&last).unwrap();
    bytes.truncate(bytes.len() - 7);
    std::fs::write(&last, &bytes).unwrap();
    let strict = EventLogReader::open(&dir, EventLogReadOptions::default()).unwrap();
    assert!(strict.collect::<Result<Vec<_>, _>>().is_err());
    let mut tolerant =
        EventLogReader::open(&dir, EventLogReadOptions { tolerate_torn_tail: true }).unwrap();
    let mut n = 0;
    while tolerant.next_record().unwrap().is_some() {
        n += 1;
    }
    assert_eq!(n, quotes.len() - 1);
    assert!(tolerant.torn_bytes() > 0);
}

#[test]
fn snapshot_files_round_trip() {
    let dir = scratch("snapshots");
    let header = node::catalog::catalog_header("snap").unwrap();
    let info = SnapshotInfo { seq: 805, ts: 123_456, complete: true, strategies: 2 };
    let body = vec![7u8; 1000];
    let encoded = snapshot_file::encode_snapshot(&header, &info, &body);
    let (h, i, range) = snapshot_file::decode_snapshot(&encoded).unwrap();
    assert_eq!(h, header);
    assert_eq!(i, info);
    assert_eq!(&encoded[range], &body[..]);
    snapshot_file::write_snapshot_file(&dir, info.seq, &encoded, false).unwrap();
    let listed = snapshot_file::list_snapshots(&dir);
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].seq, 805);
    assert_eq!(listed[0].path.file_name().unwrap(), "snapshot-00000000000000000805.jsnap");
    // Another build's snapshot is not restored.
    let err = snapshot_file::load_snapshot(&listed[0].path).unwrap_err();
    assert!(err.message.contains("written by build"), "{}", err.message);
    let mut corrupt = encoded.clone();
    corrupt[20] ^= 1;
    assert_eq!(
        snapshot_file::decode_snapshot(&corrupt),
        Err(kernel_core::Status::ChecksumMismatch)
    );
}

/// The fingerprint of a run directory's records: what `jarvis-rs fingerprint` computes for one
/// log file, over every segment.
fn run_fingerprint(dir: &Path) -> String {
    let reader = EventLogReader::open(dir, EventLogReadOptions::default()).unwrap();
    let mut fp = model::log::Fingerprinter::new();
    for record in reader {
        fp.add_record(&record.unwrap());
    }
    fp.finish().to_string()
}

#[test]
fn orders_run_fingerprint_is_golden() {
    let root = scratch("golden");
    let catalog = root.join("catalog");
    write_catalog(&catalog, 7);
    let out = root.join("run");
    let (_, result) = run(&catalog, &out, &["persistence.snapshot_every=100"]);
    assert_eq!(result.summary.state, NodeState::Stopped);
    let expected = include_str!("../../../../tests/golden/node_orders_seed7.fingerprint");
    assert_eq!(format!("{}\n", run_fingerprint(&out)), expected);
}
