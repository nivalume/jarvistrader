//! `jarvis._core.node`: strategies and the node (docs/architecture.md sections 4 and 7). The
//! Python package's `Node` loads a configuration into a [`PyNodeSetup`], adds strategy objects
//! (Python classes, or [`PyNativeSpec`] for a registered Rust strategy) and runs or replays it;
//! `parse_args`/`usage` are the single-file entry's command line, `run_report` the backtest
//! report of a run directory.

use std::path::{Path, PathBuf};

use kernel_core::Status;
use node::config::{env_name, Env, Param, ParamScalar, ParamValue, StrategyConfig};
use node::run_report::{OrderCounts, ReportRow};
use node::{
    build_run_report, config_hash_hex, format_errors, load_config, load_run_config, replay_run,
    report_text, run_backtest, BacktestRequest, BacktestResult, ConfigOverrides, HeaderExtras,
    NodeConfig, NodeError, Registry, ReplayOptions, RunManifest,
};
use pyo3::exceptions::{PyOSError, PyValueError};
use pyo3::prelude::*;
use pyo3::types::{PyBool, PyDict, PyInt, PyList, PyModule, PyString, PyTuple};
use strategy::Strategy;

use crate::common::{py_bool, type_error, PyField};
use crate::host::{HostOptions, PyStrategyHost, SharedStats};
use crate::log::{numpy_version, python_hash_seed, python_version};
use crate::values::PyInstrumentId;
use crate::views::decimal_at;

pub(crate) const VERSION: &str = node::build_info::VERSION;

/// A node failure as a Python exception: I/O as `OSError`, the rest as `ValueError`.
fn node_error(e: &NodeError) -> PyErr {
    match e.status {
        Status::IoError => PyOSError::new_err(e.message.clone()),
        _ => PyValueError::new_err(e.message.clone()),
    }
}

// ---- strategy parameters -----------------------------------------------------------------------

fn scalar_to_py<'py>(py: Python<'py>, s: &ParamScalar) -> PyResult<Bound<'py, PyAny>> {
    Ok(match s {
        ParamScalar::Bool(b) => py_bool(py, *b),
        ParamScalar::Int(i) => i.into_pyobject(py)?.into_any(),
        ParamScalar::Str(s) => PyString::new(py, s).into_any(),
    })
}

fn params_to_py<'py>(py: Python<'py>, params: &[Param]) -> PyResult<Bound<'py, PyDict>> {
    let out = PyDict::new(py);
    for p in params {
        match &p.value {
            ParamValue::Scalar(s) => out.set_item(&p.key, scalar_to_py(py, s)?)?,
            ParamValue::List(items) => {
                let list = PyList::empty(py);
                for s in items {
                    list.append(scalar_to_py(py, s)?)?;
                }
                out.set_item(&p.key, list)?;
            }
        }
    }
    Ok(out)
}

/// bool, int, str or `decimal.Decimal` (kept exact, as text); floats are refused.
fn scalar_from_py(obj: &Bound<'_, PyAny>, key: &str) -> PyResult<ParamScalar> {
    if obj.is_instance_of::<PyBool>() {
        return Ok(ParamScalar::Bool(obj.extract()?));
    }
    if obj.is_instance_of::<PyInt>() {
        return Ok(ParamScalar::Int(obj.extract()?));
    }
    if obj.is_instance_of::<PyString>() {
        return Ok(ParamScalar::Str(obj.extract()?));
    }
    if obj.hasattr("as_tuple")? {
        return Ok(ParamScalar::Str(obj.str()?.to_string()));
    }
    Err(type_error(key, "bool, int, str or decimal.Decimal (floats are not accepted)", obj))
}

fn params_from_py(params: &Bound<'_, PyDict>) -> PyResult<Vec<Param>> {
    let mut out = Vec::with_capacity(params.len());
    for (k, v) in params.iter() {
        let key: String = k.extract()?;
        let value = if v.is_instance_of::<PyList>() || v.is_instance_of::<PyTuple>() {
            let mut items = Vec::new();
            for item in v.try_iter()? {
                items.push(scalar_from_py(&item?, &key)?);
            }
            ParamValue::List(items)
        } else {
            ParamValue::Scalar(scalar_from_py(&v, &key)?)
        };
        out.push(Param { key, value });
    }
    out.sort_by(|a, b| a.key.cmp(&b.key));
    Ok(out)
}

fn entry_dict<'py>(py: Python<'py>, e: &StrategyConfig) -> PyResult<Bound<'py, PyDict>> {
    let d = PyDict::new(py);
    d.set_item("id", &e.id)?;
    d.set_item("impl", &e.impl_)?;
    let instruments = PyList::empty(py);
    for id in &e.instruments {
        instruments.append(PyInstrumentId(*id))?;
    }
    d.set_item("instruments", instruments)?;
    d.set_item("params", params_to_py(py, &e.params)?)?;
    Ok(d)
}

// ---- NativeSpec --------------------------------------------------------------------------------

/// A registered native (Rust) strategy and its parameters, for `Node.add_native_strategy`.
#[pyclass(frozen, name = "NativeSpec", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyNativeSpec {
    name: String,
    entry: StrategyConfig,
}

#[pymethods]
impl PyNativeSpec {
    #[new]
    #[pyo3(signature = (name, id, params = None))]
    fn new(name: String, id: String, params: Option<&Bound<'_, PyDict>>) -> PyResult<Self> {
        let entry = StrategyConfig {
            id,
            impl_: format!("rust:{name}"),
            instruments: Vec::new(),
            params: params.map(params_from_py).transpose()?.unwrap_or_default(),
        };
        Ok(Self { name, entry })
    }
    #[getter]
    fn name(&self) -> &str {
        &self.name
    }
    #[getter]
    fn id(&self) -> &str {
        &self.entry.id
    }
    fn __repr__(&self) -> String {
        format!("NativeSpec({:?}, {:?})", self.name, self.entry.id)
    }
}

// ---- the strategies of one run -----------------------------------------------------------------

/// Hosts and native strategies in node order, with the hosts' counters.
struct Assembly {
    strategies: Vec<Box<dyn Strategy>>,
    stats: Vec<(String, SharedStats)>,
}

fn assemble(
    config: &NodeConfig,
    strategies: &Bound<'_, PyAny>,
    replaying: bool,
) -> PyResult<Assembly> {
    let options = HostOptions {
        measure_overruns: !replaying && config.node.env != Env::Backtest,
        budget_ns: config.python.callback_budget_us.saturating_mul(1000),
        overrun_limit: config.python.overrun_limit,
    };
    let registry = Registry::with_builtins();
    let mut out = Assembly { strategies: Vec::new(), stats: Vec::new() };
    for item in strategies.try_iter()? {
        let item = item?;
        if let Ok(spec) = item.cast::<PyNativeSpec>() {
            let spec = spec.get();
            match registry.create(&spec.entry) {
                Ok(s) => out.strategies.push(s),
                Err(e) if e.status == Status::NotFound => {
                    return Err(PyValueError::new_err(format!(
                        "no C++ strategy named '{}' is registered in this build (the Rust tree \
                         has no C++ strategies; its native strategies: {})",
                        spec.name,
                        registry.names().collect::<Vec<_>>().join(", ")
                    )));
                }
                Err(e) => return Err(node_error(&e)),
            }
        } else {
            let host = PyStrategyHost::new(&item, options)?;
            out.stats.push((host.id().to_string(), host.stats()));
            out.strategies.push(Box::new(host));
        }
    }
    Ok(out)
}

fn stats_list<'py>(
    py: Python<'py>,
    stats: &[(String, SharedStats)],
) -> PyResult<Bound<'py, PyList>> {
    let out = PyList::empty(py);
    for (id, shared) in stats {
        let s = shared.get();
        let d = PyDict::new(py);
        d.set_item("id", id)?;
        d.set_item("calls", s.calls)?;
        d.set_item("total_ns", s.total_ns)?;
        d.set_item("max_ns", s.max_ns)?;
        d.set_item("overruns", s.overruns)?;
        d.set_item("escapes", s.escapes)?;
        out.append(d)?;
    }
    Ok(out)
}

fn header_extras(py: Python<'_>) -> PyResult<HeaderExtras> {
    Ok(HeaderExtras {
        python_version: python_version(py)?,
        numpy_version: numpy_version(py),
        python_hash_seed: python_hash_seed(py)?,
    })
}

fn summary_dict<'py>(py: Python<'py>, r: &BacktestResult) -> PyResult<Bound<'py, PyDict>> {
    let s = &r.summary;
    let d = PyDict::new(py);
    d.set_item(
        "directory",
        r.directory.as_ref().map_or_else(String::new, |p| p.display().to_string()),
    )?;
    d.set_item("data_events", s.data_events)?;
    d.set_item("skipped", s.skipped)?;
    d.set_item("inputs", s.inputs)?;
    d.set_item("outputs", s.outputs)?;
    d.set_item("batches", s.batches)?;
    d.set_item("timers", s.timers)?;
    d.set_item("strategy_errors", s.strategy_errors)?;
    d.set_item("venue_answers", s.venue_answers)?;
    d.set_item("halted", s.halted)?;
    d.set_item("left_open", s.left_open)?;
    d.set_item("snapshots", s.snapshots)?;
    d.set_item("snapshot_failures", s.snapshot_failures)?;
    d.set_item("state", s.state.as_str())?;
    d.set_item("first_ts", s.first_ts.value())?;
    d.set_item("last_ts", s.last_ts.value())?;
    Ok(d)
}

// ---- NodeSetup ---------------------------------------------------------------------------------

/// A loaded node configuration (and, for a run, the manifest saved with it).
#[pyclass(frozen, name = "NodeSetup", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyNodeSetup {
    config: NodeConfig,
    manifest: RunManifest,
}

#[pymethods]
impl PyNodeSetup {
    /// Loads `path` with `--env` and `--set` overrides.
    #[staticmethod]
    #[pyo3(signature = (path, env = None, sets = None))]
    fn load(path: &str, env: Option<String>, sets: Option<Vec<String>>) -> PyResult<Self> {
        let overrides = ConfigOverrides { env, sets: sets.unwrap_or_default() };
        let (config, text) = load_config(Path::new(path), &overrides).map_err(|errors| {
            if errors.len() == 1 && errors[0].path.is_empty() && errors[0].line == 0 {
                PyOSError::new_err(errors[0].message.clone())
            } else {
                PyValueError::new_err(format_errors(path, &errors))
            }
        })?;
        let manifest = RunManifest {
            config_text: text,
            source: path.to_string(),
            overrides,
            resumed_from: String::new(),
        };
        Ok(Self { config, manifest })
    }
    /// The configuration a run directory was recorded with, overrides applied.
    #[staticmethod]
    fn load_run(directory: &str) -> PyResult<Self> {
        let config = load_run_config(Path::new(directory)).map_err(|e| node_error(&e))?;
        Ok(Self { config, manifest: RunManifest::default() })
    }
    #[getter]
    fn node_id(&self) -> &str {
        &self.config.node.id
    }
    #[getter]
    fn env(&self) -> &'static str {
        env_name(self.config.node.env)
    }
    #[getter]
    fn seed(&self) -> u64 {
        self.config.node.seed
    }
    #[getter]
    fn strict_determinism(&self) -> bool {
        self.config.node.strict_determinism
    }
    #[getter]
    fn config_hash(&self) -> String {
        config_hash_hex(&self.config)
    }
    /// The `[[strategies]]` entries: id, impl, instruments, params.
    #[getter]
    fn strategies<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyList>> {
        let out = PyList::empty(py);
        for e in &self.config.strategies {
            out.append(entry_dict(py, e)?)?;
        }
        Ok(out)
    }
    /// Runs a backtest with `strategies` (Python objects and `NativeSpec`s) and returns the run
    /// summary. Sandbox and live sessions need the live layer (docs/rust-plan.md R5).
    #[pyo3(signature = (strategies, out = None, run_for = None))]
    fn run<'py>(
        &self,
        py: Python<'py>,
        strategies: &Bound<'py, PyAny>,
        out: Option<String>,
        run_for: Option<f64>,
    ) -> PyResult<Bound<'py, PyDict>> {
        let _ = run_for;
        if self.config.node.env != Env::Backtest {
            return Err(PyValueError::new_err(
                "node.env = \"sandbox\" or \"live\" needs the live shell, which this build of \
                 jarvis leaves out (the Rust tree's live layer arrives with docs/rust-plan.md R5)",
            ));
        }
        let assembly = assemble(&self.config, strategies, false)?;
        let request = BacktestRequest {
            manifest: self.manifest.clone(),
            out: out.map(PathBuf::from),
            extras: header_extras(py)?,
        };
        let result = run_backtest(&self.config, &request, assembly.strategies)
            .map_err(|e| node_error(&e))?;
        let d = summary_dict(py, &result)?;
        d.set_item("strategies", stats_list(py, &assembly.stats)?)?;
        Ok(d)
    }
    /// Replays `directory` with `strategies` and returns the report.
    #[pyo3(signature = (directory, strategies, until = None, dump_state = false, from_snapshot = None))]
    fn replay<'py>(
        &self,
        py: Python<'py>,
        directory: &str,
        strategies: &Bound<'py, PyAny>,
        until: Option<u64>,
        dump_state: bool,
        from_snapshot: Option<String>,
    ) -> PyResult<Bound<'py, PyDict>> {
        let assembly = assemble(&self.config, strategies, true)?;
        let mut options = ReplayOptions::new();
        options.until = until;
        options.dump_state = dump_state;
        options.from_snapshot = from_snapshot.map(PathBuf::from);
        let report = replay_run(Path::new(directory), &self.config, assembly.strategies, &options)
            .map_err(|e| node_error(&e))?;
        let d = PyDict::new(py);
        d.set_item("inputs", report.inputs)?;
        d.set_item("outputs", report.outputs)?;
        match &report.divergence {
            Some(div) => {
                let x = PyDict::new(py);
                x.set_item("seq", div.seq)?;
                x.set_item("recorded", &div.recorded)?;
                x.set_item("replayed", &div.replayed)?;
                d.set_item("divergence", x)?;
            }
            None => d.set_item("divergence", py.None())?,
        }
        d.set_item("state", &report.state)?;
        d.set_item("snapshots_checked", report.snapshots_checked)?;
        d.set_item("start_seq", report.start_seq)?;
        Ok(d)
    }
}

// ---- the command line ---------------------------------------------------------------------------

#[derive(Debug, Default)]
struct NodeArgs {
    config: String,
    env: Option<String>,
    sets: Vec<String>,
    out: String,
    replay: String,
    until: Option<u64>,
    run_for: Option<u64>,
    dump_state: bool,
    from_snapshot: String,
    help: bool,
}

fn takes_value(arg: &str) -> bool {
    matches!(
        arg,
        "--config"
            | "--env"
            | "--set"
            | "--out"
            | "--replay"
            | "--until"
            | "--run-for"
            | "--from-snapshot"
    )
}

fn assign(arg: &str, value: &str, out: &mut NodeArgs) -> Result<(), String> {
    match arg {
        "--config" => out.config = value.to_string(),
        "--env" => out.env = Some(value.to_string()),
        "--set" => out.sets.push(value.to_string()),
        "--out" => out.out = value.to_string(),
        "--replay" => out.replay = value.to_string(),
        "--from-snapshot" => out.from_snapshot = value.to_string(),
        "--run-for" => match value.parse::<u64>() {
            Ok(seconds) if seconds > 0 => out.run_for = Some(seconds),
            _ => return Err("--run-for needs a positive number of seconds".to_string()),
        },
        _ => match value.parse::<u64>() {
            Ok(seq) => out.until = Some(seq),
            Err(_) => return Err("--until needs a sequence number".to_string()),
        },
    }
    Ok(())
}

/// Exactly one of `--config` and `--replay`, with the options that belong to it.
fn consistent(out: &NodeArgs) -> Result<(), String> {
    if out.config.is_empty() == out.replay.is_empty() {
        return Err("give either --config FILE or --replay RUN_DIR".to_string());
    }
    let run_options =
        out.env.is_some() || !out.sets.is_empty() || !out.out.is_empty() || out.run_for.is_some();
    if !out.replay.is_empty() && run_options {
        return Err("--replay takes the configuration from the run directory (no --env, --set, \
                    --out or --run-for)"
            .to_string());
    }
    if out.replay.is_empty()
        && (out.until.is_some() || out.dump_state || !out.from_snapshot.is_empty())
    {
        return Err("--until, --dump-state and --from-snapshot apply to --replay".to_string());
    }
    Ok(())
}

fn parse_node_args(args: &[String]) -> Result<NodeArgs, String> {
    let mut out = NodeArgs::default();
    let mut i = 0;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--help" || arg == "-h" {
            out.help = true;
        } else if arg == "--dump-state" {
            out.dump_state = true;
        } else if !takes_value(arg) {
            return Err(format!("unknown argument {arg}"));
        } else if i + 1 >= args.len() {
            return Err(format!("missing value for {arg}"));
        } else {
            i += 1;
            assign(arg, &args[i], &mut out)?;
        }
        i += 1;
    }
    if !out.help {
        consistent(&out)?;
    }
    Ok(out)
}

/// Parses the node command line (the one `jarvis.main` uses).
#[pyfunction]
#[allow(clippy::needless_pass_by_value)] // extracted from the Python list
fn parse_args(py: Python<'_>, args: Vec<String>) -> PyResult<Bound<'_, PyDict>> {
    let parsed = parse_node_args(&args).map_err(PyValueError::new_err)?;
    let d = PyDict::new(py);
    d.set_item("config", &parsed.config)?;
    d.set_item("env", &parsed.env)?;
    d.set_item("sets", &parsed.sets)?;
    d.set_item("out", &parsed.out)?;
    d.set_item("replay", &parsed.replay)?;
    d.set_item("until", parsed.until)?;
    d.set_item("run_for", parsed.run_for)?;
    d.set_item("dump_state", parsed.dump_state)?;
    d.set_item("from_snapshot", &parsed.from_snapshot)?;
    d.set_item("help", parsed.help)?;
    Ok(d)
}

#[pyfunction]
fn usage(program: &str) -> String {
    format!(
        "usage:\n  {program} --config FILE [--env ENV] [--set path=value]... [--out DIR] \
         [--run-for SECONDS]\n  {program} --replay RUN_DIR [--from-snapshot FILE] [--until SEQ] \
         [--dump-state]\n\n--config runs the node and writes the run log to --out or \
         persistence.dir; a sandbox\nor live node runs until SIGINT or SIGTERM, or for --run-for \
         seconds. --replay\nrecomputes a run's outputs from its inputs and reports the first \
         divergence (exit\ncode 3); the run's EngineState snapshots must match the replayed \
         state. With\n--from-snapshot the replay starts from that snapshot instead of the first \
         input.\n"
    )
}

/// Names of the native strategies this build registers (`impl = "rust:<name>"`).
#[pyfunction]
fn registered_strategies() -> Vec<String> {
    Registry::with_builtins().names().map(str::to_string).collect()
}

/// C++ strategy plugins belong to the C++ tree; the Rust tree registers strategies in code.
#[pyfunction]
fn load_native(path: &str) -> PyResult<Vec<String>> {
    let p = Path::new(path);
    // A bare library name (`libm.so.6`) is the loader's to find; a path must exist.
    if p.parent().is_some_and(|d| !d.as_os_str().is_empty()) && !p.exists() {
        return Err(PyOSError::new_err(format!("cannot load {path}: no such file")));
    }
    Err(PyValueError::new_err(format!(
        "{path} is not a jarvis strategy plugin: this build of jarvis (the Rust tree) has no C++ \
         strategy plugins; native strategies are registered in its Registry (rust:<name>)"
    )))
}

// ---- the report ---------------------------------------------------------------------------------

fn counts_dict<'py>(py: Python<'py>, c: &OrderCounts) -> PyResult<Bound<'py, PyDict>> {
    let d = PyDict::new(py);
    d.set_item("submitted", c.submitted)?;
    d.set_item("denied", c.denied)?;
    d.set_item("accepted", c.accepted)?;
    d.set_item("rejected", c.rejected)?;
    d.set_item("canceled", c.canceled)?;
    d.set_item("expired", c.expired)?;
    d.set_item("filled", c.filled)?;
    d.set_item("modifies", c.modifies)?;
    d.set_item("cancels", c.cancels)?;
    d.set_item("modify_rejected", c.modify_rejected)?;
    d.set_item("cancel_rejected", c.cancel_rejected)?;
    d.set_item("refused", c.refused)?;
    Ok(d)
}

fn optional_py<'py, T: PyField>(py: Python<'py>, v: Option<&T>) -> PyResult<Bound<'py, PyAny>> {
    match v {
        Some(v) => v.to_py(py),
        None => Ok(py.None().into_bound(py)),
    }
}

fn row_dict<'py>(py: Python<'py>, r: &ReportRow) -> PyResult<Bound<'py, PyDict>> {
    let d = PyDict::new(py);
    d.set_item("strategy", &r.strategy)?;
    d.set_item("instrument", &r.instrument)?;
    d.set_item("fills", r.fills)?;
    d.set_item("maker_fills", r.maker_fills)?;
    d.set_item("taker_fills", r.taker_fills)?;
    d.set_item("bought", r.bought.to_py(py)?)?;
    d.set_item("sold", r.sold.to_py(py)?)?;
    d.set_item("notional", r.notional.to_py(py)?)?;
    d.set_item("commission", r.commission.to_py(py)?)?;
    d.set_item("funding", r.funding.to_py(py)?)?;
    d.set_item("realized_pnl", r.realized.to_py(py)?)?;
    d.set_item("unrealized_pnl", r.unrealized.to_py(py)?)?;
    d.set_item("net_pnl", r.net.to_py(py)?)?;
    d.set_item("position", decimal_at(py, r.position_raw, r.size_precision)?)?;
    d.set_item("avg_px_open", optional_py(py, r.avg_px_open.as_ref())?)?;
    d.set_item("valuation", optional_py(py, r.valuation.as_ref())?)?;
    d.set_item("valuation_source", &r.valuation_source)?;
    Ok(d)
}

/// The backtest report of a run directory (see `jarvis.report.RunReport`).
#[pyfunction]
fn run_report<'py>(py: Python<'py>, directory: &str) -> PyResult<Bound<'py, PyDict>> {
    let r = build_run_report(Path::new(directory)).map_err(|e| node_error(&e))?;
    let d = PyDict::new(py);
    d.set_item("text", report_text(&r))?;
    d.set_item("directory", &r.directory)?;
    d.set_item("node_id", &r.node_id)?;
    d.set_item("env", &r.env)?;
    d.set_item("seed", r.seed)?;
    d.set_item("catalog", &r.catalog)?;
    d.set_item("range", &r.range)?;
    d.set_item("streams", &r.streams)?;
    d.set_item("instruments", &r.instruments)?;
    d.set_item("book", &r.book)?;
    d.set_item("venue", &r.venue)?;
    d.set_item("fill_model", &r.fill_model)?;
    d.set_item("fee_schedule", &r.fee_schedule)?;
    let start = PyList::empty(py);
    for m in &r.starting_balances {
        start.append(m.to_py(py)?)?;
    }
    let end = PyList::empty(py);
    for m in &r.ending_balances {
        end.append(m.to_py(py)?)?;
    }
    d.set_item("starting_balances", start)?;
    d.set_item("ending_balances", end)?;
    let rows = PyList::empty(py);
    for row in &r.rows {
        rows.append(row_dict(py, row)?)?;
    }
    d.set_item("rows", rows)?;
    let orders = PyDict::new(py);
    for (strategy, counts) in &r.orders {
        orders.set_item(strategy, counts_dict(py, counts)?)?;
    }
    d.set_item("orders", orders)?;
    d.set_item("first_ts", r.first_ts)?;
    d.set_item("last_ts", r.last_ts)?;
    Ok(d)
}

pub(crate) fn build(py: Python<'_>) -> PyResult<Bound<'_, PyModule>> {
    let m = PyModule::new(py, "node")?;
    m.add("__doc__", "Strategies and the node (docs/architecture.md sections 4 and 7)")?;
    crate::views::add_classes(py, &m)?;
    crate::host::add_classes(&m)?;
    m.add_class::<PyNativeSpec>()?;
    m.add_class::<PyNodeSetup>()?;
    m.add_function(wrap_pyfunction!(parse_args, &m)?)?;
    m.add_function(wrap_pyfunction!(usage, &m)?)?;
    m.add_function(wrap_pyfunction!(registered_strategies, &m)?)?;
    m.add_function(wrap_pyfunction!(load_native, &m)?)?;
    m.add_function(wrap_pyfunction!(run_report, &m)?)?;
    m.add("has_live", false)?;
    // Parent orders and execution algorithms arrive with docs/rust-plan.md R3's second step.
    m.add("has_exec_algos", false)?;
    Ok(m)
}
