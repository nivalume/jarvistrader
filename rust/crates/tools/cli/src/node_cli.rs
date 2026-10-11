//! The node subcommands of `jarvis-rs` (docs/architecture.md section 17):
//!
//! ```text
//! jarvis-rs config FILE [--env ENV] [--set path=value]... [--out FILE]
//! jarvis-rs backtest --config FILE [--env ENV] [--set path=value]... [--out DIR]
//! jarvis-rs replay RUN_DIR [--from-snapshot FILE] [--until SEQ] [--dump-state]
//! jarvis-rs report RUN_DIR [--out FILE]
//! jarvis-rs snapshot FILE
//! jarvis-rs catalog --seed N --out DIR [--instrument]
//! ```
//!
//! Exit status: 0 success, 1 failure, 2 usage error, 3 replay divergence.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use corpus::golden;
use kernel_core::sha256::{hex, Sha256};
use node::snapshot_file;
use node::{
    build_run_report, config_report, format_errors, load_config, load_run_config, replay_run,
    report_text, run_backtest, BacktestRequest, BacktestResult, ConfigOverrides, Registry,
    ReplayOptions, ReplayReport, RunManifest,
};

pub(crate) const EXIT_FAILED: u8 = 1;
pub(crate) const EXIT_USAGE: u8 = 2;
pub(crate) const EXIT_DIVERGENCE: u8 = 3;

/// Positional arguments, `--name value` options and `--flag`s of one subcommand.
#[derive(Debug, Default)]
pub(crate) struct Args {
    pub positional: Vec<String>,
    pub options: Vec<(String, String)>,
    pub flags: Vec<String>,
}

impl Args {
    /// `values` take a value, `flags` do not; anything else starting with `--` is an error.
    pub(crate) fn parse(args: &[String], values: &[&str], flags: &[&str]) -> Result<Self, String> {
        let mut out = Self::default();
        let mut i = 0;
        while i < args.len() {
            let a = &args[i];
            if values.contains(&a.as_str()) {
                let v = args.get(i + 1).ok_or_else(|| format!("missing value for {a}"))?;
                out.options.push((a.clone(), v.clone()));
                i += 2;
                continue;
            }
            if flags.contains(&a.as_str()) {
                out.flags.push(a.clone());
            } else if a.starts_with("--") {
                return Err(format!("unknown option {a}"));
            } else {
                out.positional.push(a.clone());
            }
            i += 1;
        }
        Ok(out)
    }
    pub(crate) fn option(&self, name: &str) -> Option<&str> {
        self.options.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }
    pub(crate) fn all(&self, name: &str) -> Vec<String> {
        self.options.iter().filter(|(k, _)| k == name).map(|(_, v)| v.clone()).collect()
    }
    pub(crate) fn flag(&self, name: &str) -> bool {
        self.flags.iter().any(|f| f == name)
    }
    fn overrides(&self) -> ConfigOverrides {
        ConfigOverrides { env: self.option("--env").map(str::to_string), sets: self.all("--set") }
    }
}

fn failed(message: &str) -> ExitCode {
    eprintln!("jarvis-rs: {message}");
    ExitCode::from(EXIT_FAILED)
}

fn usage(message: &str) -> ExitCode {
    eprintln!("jarvis-rs: {message}");
    ExitCode::from(EXIT_USAGE)
}

fn write_out(out: Option<&str>, text: &str) -> ExitCode {
    let Some(path) = out else {
        print!("{text}");
        return ExitCode::SUCCESS;
    };
    match std::fs::write(path, text) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => failed(&format!("cannot write {path}: {e}")),
    }
}

/// `config FILE [--env ENV] [--set path=value]... [--out FILE]`: the canonical text and hash.
pub(crate) fn config(args: &[String]) -> ExitCode {
    let args = match Args::parse(args, &["--env", "--set", "--out"], &[]) {
        Ok(a) => a,
        Err(e) => return usage(&e),
    };
    let [path] = args.positional.as_slice() else { return usage("config needs one TOML file") };
    match load_config(Path::new(path), &args.overrides()) {
        Ok((config, _)) => write_out(args.option("--out"), &config_report(&config)),
        Err(errors) => failed(&format_errors(path, &errors)),
    }
}

fn load_for_run(
    path: &str,
    overrides: ConfigOverrides,
) -> Result<(node::NodeConfig, RunManifest), String> {
    let (config, text) =
        load_config(Path::new(path), &overrides).map_err(|errors| format_errors(path, &errors))?;
    let manifest = RunManifest {
        config_text: text,
        source: path.to_string(),
        overrides,
        resumed_from: String::new(),
    };
    Ok((config, manifest))
}

pub(crate) fn print_backtest(result: &BacktestResult) -> ExitCode {
    let s = &result.summary;
    println!(
        "run: {}",
        result
            .directory
            .as_ref()
            .map_or_else(|| "(not recorded)".to_string(), |d| d.display().to_string())
    );
    println!(
        "inputs: {} (data {}, venue answers {}, batches {}, timers {}, strategy errors {})",
        s.inputs, s.data_events, s.venue_answers, s.batches, s.timers, s.strategy_errors
    );
    println!("outputs: {}", s.outputs);
    println!("skipped: {}", s.skipped);
    println!("state: {:?}{}", s.state, if s.halted { " (halted by a strategy error)" } else { "" });
    if s.snapshots != 0 || s.snapshot_failures != 0 {
        println!("snapshots: {} ({} failed)", s.snapshots, s.snapshot_failures);
    }
    if s.halted {
        ExitCode::from(EXIT_FAILED)
    } else {
        ExitCode::SUCCESS
    }
}

/// `backtest --config FILE [--env ENV] [--set path=value]... [--out DIR]`.
pub(crate) fn backtest(args: &[String]) -> ExitCode {
    let args = match Args::parse(args, &["--config", "--env", "--set", "--out"], &[]) {
        Ok(a) => a,
        Err(e) => return usage(&e),
    };
    let Some(path) = args.option("--config") else { return usage("backtest needs --config FILE") };
    if !args.positional.is_empty() {
        return usage("backtest takes options only");
    }
    let (config, manifest) = match load_for_run(path, args.overrides()) {
        Ok(v) => v,
        Err(e) => return failed(&e),
    };
    let strategies = match Registry::with_builtins().create_all(&config.strategies) {
        Ok(s) => s,
        Err(e) => return failed(&e.message),
    };
    let request = BacktestRequest {
        manifest,
        out: args.option("--out").map(PathBuf::from),
        ..BacktestRequest::default()
    };
    match run_backtest(&config, &request, strategies) {
        Ok(result) => print_backtest(&result),
        Err(e) => failed(&e.message),
    }
}

pub(crate) fn print_replay(directory: &str, report: &ReplayReport) -> ExitCode {
    match &report.divergence {
        Some(d) => {
            println!("ReplayDivergence in {directory} at seq {}", d.seq);
            println!("  recorded: {}", d.recorded);
            println!("  replayed: {}", d.replayed);
        }
        None => println!(
            "replayed {directory}: {} inputs, {} outputs, no divergence",
            report.inputs, report.outputs
        ),
    }
    if report.start_seq != 0 {
        println!("started from the snapshot after seq {}", report.start_seq);
    }
    if report.snapshots_checked != 0 {
        println!("snapshots matching the replayed state: {}", report.snapshots_checked);
    }
    print!("{}", report.state);
    if report.divergence.is_some() {
        ExitCode::from(EXIT_DIVERGENCE)
    } else {
        ExitCode::SUCCESS
    }
}

/// `replay RUN_DIR [--from-snapshot FILE] [--until SEQ] [--dump-state]`.
pub(crate) fn replay(args: &[String]) -> ExitCode {
    let args = match Args::parse(args, &["--until", "--from-snapshot"], &["--dump-state"]) {
        Ok(a) => a,
        Err(e) => return usage(&e),
    };
    let [directory] = args.positional.as_slice() else {
        return usage("replay needs one run directory");
    };
    let mut options = ReplayOptions::new();
    options.dump_state = args.flag("--dump-state");
    options.from_snapshot = args.option("--from-snapshot").map(PathBuf::from);
    if let Some(until) = args.option("--until") {
        match until.parse::<u64>() {
            Ok(seq) => options.until = Some(seq),
            Err(_) => return usage("--until needs a sequence number"),
        }
    }
    let dir = Path::new(directory);
    let config = match load_run_config(dir) {
        Ok(c) => c,
        Err(e) => return failed(&e.message),
    };
    let strategies = match Registry::with_builtins().create_all(&config.strategies) {
        Ok(s) => s,
        Err(e) => return failed(&e.message),
    };
    match replay_run(dir, &config, strategies, &options) {
        Ok(report) => print_replay(directory, &report),
        Err(e) => failed(&e.message),
    }
}

/// `report RUN_DIR [--out FILE]`.
pub(crate) fn report(args: &[String]) -> ExitCode {
    let args = match Args::parse(args, &["--out"], &[]) {
        Ok(a) => a,
        Err(e) => return usage(&e),
    };
    let [directory] = args.positional.as_slice() else {
        return usage("report needs one run directory");
    };
    match build_run_report(Path::new(directory)) {
        Ok(r) => write_out(args.option("--out"), &report_text(&r)),
        Err(e) => failed(&format!("report: {}", e.message)),
    }
}

/// `snapshot FILE`: where it was taken, whether it is complete, and the SHA-256 of the state.
pub(crate) fn snapshot(args: &[String]) -> ExitCode {
    let [path] = args else { return usage("snapshot needs one snapshot file") };
    let file = match std::fs::read(path) {
        Ok(f) => f,
        Err(e) => return failed(&format!("snapshot {path}: {e}")),
    };
    let (header, info, body) = match snapshot_file::decode_snapshot(&file) {
        Ok(v) => v,
        Err(s) => return failed(&format!("snapshot {path}: {s}")),
    };
    let body = &file[body];
    let config = hex(&header.config_hash);
    println!(
        "seq {}, ts {}, strategies {}, {}, config {}, build \"{}\"",
        info.seq,
        info.ts,
        info.strategies,
        if info.complete { "complete" } else { "incomplete" },
        String::from_utf8_lossy(&config[..16]),
        header.build.as_str()
    );
    println!("state {} bytes={}", String::from_utf8_lossy(&hex(&Sha256::digest(body))), body.len());
    ExitCode::SUCCESS
}

/// `catalog --seed N --out DIR [--instrument]`: the golden catalog (one day, BTCUSDT-PERP).
pub(crate) fn catalog(args: &[String]) -> ExitCode {
    let args = match Args::parse(args, &["--seed", "--out"], &["--instrument"]) {
        Ok(a) => a,
        Err(e) => return usage(&e),
    };
    let Some(seed) = args.option("--seed").and_then(|s| s.parse::<u64>().ok()) else {
        return usage("catalog needs --seed N");
    };
    let Some(out) = args.option("--out") else { return usage("catalog needs --out DIR") };
    let dir = Path::new(out);
    let id = golden::instrument_id();
    let streams = match golden::catalog(seed) {
        Ok(s) => s,
        Err(e) => return failed(&format!("generating the catalog: {e}")),
    };
    let mut written = 0usize;
    for stream in &streams {
        if let Err(e) =
            node::catalog::write_stream(dir, &id, stream.name, golden::DAY_NAME, &stream.records)
        {
            return failed(&format!("cannot write the catalog in {out}: {e}"));
        }
        written += stream.records.len();
    }
    if args.flag("--instrument") {
        let stream = match golden::instrument_stream() {
            Ok(s) => s,
            Err(e) => return failed(&format!("generating the instrument definition: {e}")),
        };
        if let Err(e) =
            node::catalog::write_stream(dir, &id, stream.name, golden::DAY_NAME, &stream.records)
        {
            return failed(&format!("cannot write the instrument definition in {out}: {e}"));
        }
        written += stream.records.len();
    }
    println!("{written} records to {out}");
    ExitCode::SUCCESS
}
