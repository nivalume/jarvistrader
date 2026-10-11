//! The backtest node (docs/architecture.md section 4): configuration -> catalog sources -> merge
//! -> driver -> engine, with every input and output written to the run log.

use std::path::{Path, PathBuf};

use backtest::driver::{DriverOptions, Recorder, RunSummary, Source};
use backtest::{Driver, EventSource, MergeSource, SimConfig, VenueLoop, VenueLoopConfig};
use data::subscription::StrategyIndex;
use engine::Engine;
use kernel_core::rng::CounterRng;
use kernel_core::{DurationNanos, EventKey, FixedString, FixedVec, Result, Status, UnixNanos};
use model::account::{AccountBalance, AccountState};
use model::enums::AccountType;
use model::log::RecordBody;
use model::outputs::Output;
use model::{AccountId, Event, InstrumentId, Money, StrategyId, TraderId, Uuid4};
use portfolio::MarginModel;
use strategy::{ErrorPolicy, Kernel, KernelConfig, Strategy};

use crate::config::{
    Env, FillModel, NodeConfig, OnStrategyError, PersistenceMode, SelfTradePrevention, SimSection,
};
use crate::event_log::{EventLogOptions, EventLogReadOptions, EventLogReader, EventLogWriter};
use crate::log_source::{
    catalog_directory, day_directories, day_start, resolve_catalog, LogSource,
};
use crate::run_dir::{create_run_directory, run_header_with, HeaderExtras, RunManifest};
use crate::snapshot_file::SnapshotInfo;
use crate::{NodeError, NodeResult};

/// Kernel capacities and identities from `[node]`, `[[venues]]` and `[risk]`.
#[must_use]
pub fn kernel_config(config: &NodeConfig) -> KernelConfig {
    // Orders, their fill records (16 per order on average) and the identities they carry: the
    // ClientOrderId prefix is the node id, the trader "<NODE>-001", the account "<VENUE>-001".
    const TRADES_PER_ORDER: u64 = 16;
    let mut k = KernelConfig {
        instruments: config.node.capacity.instruments,
        strategies: config.node.capacity.strategies.min(u32::from(u16::MAX)) as u16,
        timers: config.node.capacity.timers,
        batch: config.node.capacity.batch,
        seed: config.node.seed,
        ..KernelConfig::default()
    };
    k.trading.orders = config.node.capacity.orders;
    k.trading.trades = (u64::from(config.node.capacity.orders) * TRADES_PER_ORDER)
        .min(u64::from(u32::MAX / 2)) as u32;
    k.trading.node_tag = FixedString::from_text(&config.node.id).unwrap_or_default();
    if !config.node.id.is_empty() {
        if let Ok(id) = TraderId::new(&format!("{}-001", config.node.id.to_uppercase())) {
            k.trading.trader_id = id;
        }
    }
    if let Some(venue) = config.venues.first() {
        if let Ok(id) = AccountId::new(&format!("{}-001", venue.id.to_uppercase())) {
            k.trading.account_id = id;
        }
        if venue.leverage > 0 {
            if let Ok(m) = MarginModel::leveraged(venue.leverage) {
                k.trading.margin = m;
            }
        }
    }
    let r = &config.risk;
    let rc = &mut k.trading.risk;
    rc.initial_state = r.initial_state;
    rc.max_order_notional = r.max_order_notional;
    rc.max_position_notional = r.max_position_notional;
    rc.daily_loss_limit = r.daily_loss_limit;
    rc.daily_loss_halt = r.daily_loss_halt;
    rc.max_drawdown = r.max_drawdown;
    rc.price_band_bps = r.price_band_bps;
    rc.max_open_orders = r.max_open_orders;
    rc.orders_per_10s = r.orders_per_10s;
    rc.orders_per_minute = r.orders_per_minute;
    rc.margin_ratio_bps = r.margin_ratio_bps;
    rc.check_margin = r.check_margin;
    if config.node.env == Env::Live {
        rc.countdown_cancel_ms = r.countdown_cancel_all_ms;
    }
    k
}

#[must_use]
pub fn error_policy(config: &NodeConfig) -> ErrorPolicy {
    match config.risk.on_strategy_error {
        OnStrategyError::HaltStrategy => ErrorPolicy::HaltStrategy,
        OnStrategyError::HaltNode => ErrorPolicy::HaltNode,
        OnStrategyError::Ignore => ErrorPolicy::Ignore,
    }
}

/// Strategy `i` issues orders under `[[strategies]][i].id` (a `StrategyId`, `<name>-<tag>`); the
/// default `strategy-00<i+1>` stays for entries without a valid one. A strategy whose entry lists
/// instruments may trade only those.
pub fn name_strategies(config: &NodeConfig, kernel: &mut Kernel) {
    let strategies = kernel.config().strategies as usize;
    for (i, entry) in config.strategies.iter().enumerate().take(strategies) {
        let s = i as u16;
        if let Ok(id) = StrategyId::new(&entry.id) {
            kernel.trading.set_strategy_id(StrategyIndex(s), id);
        }
        if !entry.instruments.is_empty() {
            kernel.trading.risk.restrict(s);
            for instrument in &entry.instruments {
                if let Ok(slot) = kernel.instruments.intern(*instrument) {
                    kernel.trading.risk.allow(s, slot.0);
                }
            }
        }
    }
}

/// The last definition of `instrument` in the catalog day directory `directory`.
fn last_definition(directory: &Path, instrument: &InstrumentId) -> Result<Option<Event>> {
    let reader = EventLogReader::open(directory, EventLogReadOptions::default())?;
    let mut out = None;
    for record in reader {
        if let RecordBody::Input(Event::Instrument(def)) = record?.body {
            if def.id == *instrument {
                out = Some(Event::Instrument(def));
            }
        }
    }
    Ok(out)
}

/// The instrument definitions found in the catalog (`{catalog}/{instrument}/instrument/{day}/`,
/// the latest day that starts no later than `data.range`, else the earliest). An instrument
/// without definitions is left out: orders for it are denied (`INSTRUMENT_UNKNOWN`).
fn load_definitions(config: &NodeConfig) -> NodeResult<Vec<Event>> {
    let mut seen: Vec<InstrumentId> = Vec::new();
    let mut out = Vec::new();
    for group in &config.data.streams {
        for instrument in &group.instruments {
            if seen.contains(instrument) {
                continue;
            }
            seen.push(*instrument);
            let root = catalog_directory(&config.data.catalog, instrument, "instrument", "");
            let days = day_directories(&root);
            let Some((mut chosen, _)) = days.first().cloned() else { continue };
            for (day, _) in &days {
                if let (Some(range), Some(start)) = (&config.data.range, day_start(day)) {
                    if range.start < start {
                        break;
                    }
                }
                chosen.clone_from(day);
            }
            let directory =
                catalog_directory(&config.data.catalog, instrument, "instrument", &chosen);
            let definition = last_definition(&directory, instrument).map_err(|s| {
                NodeError::new(
                    s,
                    format!("cannot read instrument definitions in {}: {s}", directory.display()),
                )
            })?;
            out.extend(definition);
        }
    }
    Ok(out)
}

/// The simulated venue's starting account (`[venues.sim] balances`) as an `AccountState` at `ts`.
pub fn account_preamble(config: &NodeConfig, sim: &SimSection, ts: UnixNanos) -> NodeResult<Event> {
    // A valid, reproducible UUID4 (a zero id does not decode).
    const ACCOUNT_SALT: u64 = 0xA54F_F53A_5F1D_36F1;
    let mut balances = FixedVec::with_capacity(sim.balances.len());
    for total in &sim.balances {
        let balance = AccountBalance::new(*total, Money::zero(total.currency()), *total)
            .map_err(|s| NodeError::new(s, "venues.sim.balances: invalid balance"))?;
        balances
            .push(balance)
            .map_err(|s| NodeError::new(s, "venues.sim.balances: too many balances"))?;
    }
    let rng = CounterRng::new(config.node.seed ^ ACCOUNT_SALT);
    let event_id = Uuid4::from_u64s(rng.draw(0, 0, 0), rng.draw(0, 0, 1));
    let account = AccountState::new(
        kernel_config(config).trading.account_id,
        AccountType::Margin,
        None,
        balances,
        FixedVec::with_capacity(0),
        true,
        event_id,
        ts,
        ts,
    )
    .map_err(|s| NodeError::new(s, "venues.sim.balances: cannot build the account state"))?;
    Ok(Event::AccountState(account))
}

/// What the node steps while `Syncing`, before strategies start: the instrument definitions and,
/// with a simulated venue, its starting account.
pub fn load_preamble(config: &NodeConfig) -> NodeResult<Vec<Event>> {
    let mut out = load_definitions(config)?;
    let Some(sim) = config.venues.first().and_then(|v| v.sim.as_ref()) else { return Ok(out) };
    if sim.balances.is_empty() {
        return Ok(out);
    }
    let ts = config.data.range.as_ref().map_or_else(UnixNanos::default, |r| r.start);
    out.push(account_preamble(config, sim, ts)?);
    Ok(out)
}

/// The simulated venue of `[venues.sim]`: fills, fees, latency, self-trade prevention.
pub fn venue_loop_config(
    config: &NodeConfig,
    kernel: &KernelConfig,
) -> NodeResult<VenueLoopConfig> {
    let sim = config
        .venues
        .first()
        .and_then(|v| v.sim.as_ref())
        .ok_or_else(|| NodeError::new(Status::InvalidArgument, "no [venues.sim] section"))?;
    let schedule =
        if sim.fee_schedule.is_empty() { "binance_usdm_vip0" } else { &sim.fee_schedule };
    let fees = cost::MakerTakerFees::schedule(schedule).map_err(|_| {
        NodeError::new(
            Status::InvalidArgument,
            format!(
                "venues.sim.fee.schedule: unknown schedule \"{schedule}\" (binance_usdm_vip0, \
                 binance_usdm_vip0_bnb, binance_spot_vip0, binance_spot_vip0_bnb, zero)"
            ),
        )
    })?;
    let sim_config = SimConfig {
        fill_model: match sim.fill_model {
            FillModel::TopOfBook => backtest::FillModel::TopOfBook,
            FillModel::QueuePosition => backtest::FillModel::QueuePosition,
        },
        stp: match sim.stp {
            SelfTradePrevention::None => backtest::StpMode::None,
            SelfTradePrevention::ExpireTaker => backtest::StpMode::ExpireTaker,
            SelfTradePrevention::ExpireMaker => backtest::StpMode::ExpireMaker,
            SelfTradePrevention::ExpireBoth => backtest::StpMode::ExpireBoth,
        },
        fees,
        instruments: kernel.instruments,
        strategies: kernel.strategies,
        orders: kernel.trading.orders,
        // The venue's books hold what the kernel's hold.
        book_levels: kernel.book_window_levels,
        book_overflow_levels: kernel.book_overflow_levels,
        margin: kernel.trading.margin,
        account_id: kernel.trading.account_id,
        trader_id: kernel.trading.trader_id,
        seed: config.node.seed,
        ..SimConfig::default()
    };
    Ok(VenueLoopConfig {
        sim: sim_config,
        feed: DurationNanos::new(sim.latency.feed_ns),
        outbound: DurationNanos::new(sim.latency.out_ns),
        inbound: DurationNanos::new(sim.latency.in_ns),
        jitter: DurationNanos::new(sim.latency.jitter_ns),
        seed: config.node.seed,
        start: config.data.range.as_ref().map(|r| r.start),
        end: config.data.range.as_ref().map(|r| r.end),
        ..VenueLoopConfig::default()
    })
}

/// One `LogSource` per catalog stream that `[data]` selects.
pub fn open_sources(config: &NodeConfig) -> NodeResult<Vec<Box<dyn EventSource>>> {
    let streams = resolve_catalog(&config.data)?;
    let mut sources: Vec<Box<dyn EventSource>> = Vec::with_capacity(streams.len());
    for stream in streams {
        let first = stream.days[0].clone();
        let source = LogSource::open(stream.days)
            .map_err(|s| NodeError::new(s, format!("cannot open {}: {s}", first.display())))?;
        sources.push(Box::new(source));
    }
    Ok(sources)
}

/// The driver's recorder over the run log, snapshots included.
#[derive(Debug)]
pub struct LogFileRecorder {
    pub writer: EventLogWriter,
}

impl Recorder for LogFileRecorder {
    fn record(&mut self, key: EventKey, event: &Event) -> Result<()> {
        self.writer.append(key, event)
    }
    fn emit(&mut self, key: EventKey, output: &Output) -> Result<()> {
        self.writer.append_output(key, output)
    }
    fn takes_snapshots(&self) -> bool {
        true
    }
    fn snapshot(&mut self, engine: &Engine, key: EventKey) -> Result<()> {
        let body = engine.save_state()?;
        let info = SnapshotInfo {
            seq: key.seq,
            ts: key.ts.value(),
            complete: engine.snapshot_complete(),
            strategies: engine.strategy_count() as u16,
        };
        self.writer.write_snapshot(&info, &body)
    }
}

/// `persistence.mode = "none"`: nothing is written.
#[derive(Debug, Default)]
pub struct NullRecorder;

impl Recorder for NullRecorder {
    fn record(&mut self, _: EventKey, _: &Event) -> Result<()> {
        Ok(())
    }
    fn emit(&mut self, _: EventKey, _: &Output) -> Result<()> {
        Ok(())
    }
}

#[derive(Clone, Debug, Default)]
pub struct BacktestRequest {
    /// Saved in the run directory.
    pub manifest: RunManifest,
    /// The run directory; `None` for `persistence.dir`.
    pub out: Option<PathBuf>,
    /// The Python host's versions, for the log header.
    pub extras: HeaderExtras,
}

#[derive(Clone, Debug, Default)]
pub struct BacktestResult {
    /// `None` when `persistence.mode = "none"`.
    pub directory: Option<PathBuf>,
    pub summary: RunSummary,
}

fn fault(engine: &Engine, status: Status) -> NodeError {
    NodeError::new(
        status,
        format!("the node faulted at seq {}: {status}", engine.kernel().current.seq),
    )
}

fn drive(
    config: &NodeConfig,
    request: &BacktestRequest,
    engine: &mut Engine,
    source: Source,
    options: DriverOptions,
) -> NodeResult<BacktestResult> {
    if config.persistence.mode == PersistenceMode::None {
        let mut recorder = NullRecorder;
        let summary = Driver::new(engine, source, &mut recorder, options)
            .run()
            .map_err(|s| NodeError::new(s, format!("the node faulted: {s}")))?;
        return Ok(BacktestResult { directory: None, summary });
    }
    let directory = create_run_directory(config, &request.manifest, request.out.as_deref())?;
    // A backtest writes the log on the core thread and never syncs it: async and barrier differ
    // only where commands reach a venue (section 16.2).
    let writer = EventLogWriter::open(
        &directory,
        &run_header_with(config, &request.extras),
        EventLogOptions::default(),
    )
    .map_err(|s| {
        NodeError::new(s, format!("cannot open the run log in {}", directory.display()))
    })?;
    let mut recorder = LogFileRecorder { writer };
    let run = Driver::new(engine, source, &mut recorder, options).run();
    let closed = recorder.writer.close();
    let summary = run.map_err(|s| fault(engine, s))?;
    closed.map_err(|s| {
        NodeError::new(s, format!("cannot finish the run log in {}", directory.display()))
    })?;
    Ok(BacktestResult { directory: Some(directory), summary })
}

/// Runs a backtest of `config` with `strategies` (one per `[[strategies]]` entry, or none).
pub fn run_backtest(
    config: &NodeConfig,
    request: &BacktestRequest,
    strategies: Vec<Box<dyn Strategy>>,
) -> NodeResult<BacktestResult> {
    if config.node.env != Env::Backtest {
        return Err(NodeError::new(
            Status::InvalidArgument,
            format!(
                "node.env = \"{}\" is not available yet; this build runs backtests (sandbox and \
                 live arrive with R5)",
                crate::config::env_name(config.node.env)
            ),
        ));
    }
    if strategies.len() as u64 > u64::from(config.node.capacity.strategies) {
        return Err(NodeError::new(
            Status::CapacityExceeded,
            format!(
                "the node has {} strategies but node.capacity.strategies is {}",
                strategies.len(),
                config.node.capacity.strategies
            ),
        ));
    }
    let sources = open_sources(config)?;
    let merge: Box<dyn EventSource> = Box::new(MergeSource::new(sources));
    let mut options = DriverOptions {
        snapshot_every: config.persistence.snapshot_every,
        ..DriverOptions::default()
    };
    if let Some(range) = &config.data.range {
        options.start = Some(range.start);
        options.end = Some(range.end);
    }
    options.preamble = load_preamble(config)?;
    let kernel = kernel_config(config);
    let mut engine = Engine::new(kernel, strategies, error_policy(config))
        .map_err(|s| NodeError::new(s, "cannot build the engine"))?;
    name_strategies(config, engine.kernel_mut());
    let source = if config.venues.first().is_some_and(|v| v.sim.is_some()) {
        let venue = venue_loop_config(config, engine.kernel().config())?;
        Source::Venue(VenueLoop::new(venue, merge))
    } else {
        Source::Plain(merge)
    };
    drive(config, request, &mut engine, source, options)
}
