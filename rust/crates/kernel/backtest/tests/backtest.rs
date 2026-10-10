//! The backtest layer: the simulated exchange against `specs/tla/Matching.tla` (forward trace
//! validation) and Binance's answers, the merge of sources, the lifecycle table, and a whole run
//! through the venue loop whose log replays byte for byte.

use std::cell::Cell;
use std::rc::Rc;

use backtest::driver::{DriverOptions, FingerprintRecorder, LogRecorder, Recorder, Source};
use backtest::sim::reject;
use backtest::{
    replay, Driver, EventSource, FillModel, MergeSource, ReplayError, ReplaySource, SimConfig,
    SimulatedExchange, StpMode, VecSource, VenueLoop, VenueLoopConfig,
};
use corpus::fixtures::{account, instrument_id, perpetual};
use engine::lifecycle::{next_state, Lifecycle};
use engine::Engine;
use execution::OrderIntent;
use kernel_core::{DurationNanos, EventKey, Result, UnixNanos};
use model::data::{QuoteTick, TradeTick};
use model::enums::{
    AggressorSide, LifecycleReason, LiquiditySide, NodeState, OrderSide, OrderType, TimeInForce,
};
use model::log::LogHeader;
use model::order_events::OrderEvent;
use model::outputs::{CancelOrder, ModifyOrder, Output, SubmitOrder};
use model::{ClientOrderId, Event, Price, Quantity, TradeId};
use strategy::{Cadence, Context, ErrorPolicy, KernelConfig, Strategy};
use testkit::behaviour::{
    behaviours_path, replay as replay_behaviours, BehaviourFile, Diff, Replayer, Step,
};

const PRICE: i64 = 100_000_000_000; // 100.0
const TICK: i64 = 100_000_000; // 0.1

fn px(raw: i64) -> Price {
    Price::from_raw(raw, 1).unwrap()
}
fn q9(raw: u64) -> Quantity {
    Quantity::from_raw(raw, 9).unwrap()
}
fn cid(n: u64) -> ClientOrderId {
    model::client_order_id::format("t", 1, n).unwrap()
}

fn sim(fill_model: FillModel, stp: StpMode) -> SimulatedExchange {
    let mut s = SimulatedExchange::new(SimConfig {
        fill_model,
        stp,
        instruments: 2,
        orders: 8,
        fees: cost::MakerTakerFees::schedule("binance_usdm_vip0").unwrap(),
        ..SimConfig::default()
    });
    s.on_data(&Event::Instrument(perpetual(9)), UnixNanos::new(1)).unwrap();
    s
}

fn quote_at(bid: i64, bid_size: u64, ask: i64, ask_size: u64, ts: u64) -> Event {
    Event::QuoteTick(
        QuoteTick::new(
            instrument_id(),
            px(bid),
            px(ask),
            q9(bid_size),
            q9(ask_size),
            UnixNanos::new(ts),
            UnixNanos::new(ts),
        )
        .unwrap(),
    )
}
fn trade_at(price: i64, size: u64, aggressor: AggressorSide, ts: u64) -> Event {
    Event::TradeTick(
        TradeTick::new(
            instrument_id(),
            px(price),
            q9(size),
            aggressor,
            TradeId::new(&format!("T{ts}")).unwrap(),
            UnixNanos::new(ts),
            UnixNanos::new(ts),
        )
        .unwrap(),
    )
}
fn submit(
    n: u64,
    side: OrderSide,
    qty: u64,
    price: Option<i64>,
    tif: TimeInForce,
    post_only: bool,
) -> Output {
    Output::SubmitOrder(SubmitOrder {
        strategy_index: 0,
        client_order_id: cid(n),
        instrument_id: instrument_id(),
        order_side: side,
        order_type: if price.is_some() { OrderType::Limit } else { OrderType::Market },
        quantity: q9(qty),
        price: price.map(px),
        time_in_force: tif,
        post_only,
        reduce_only: false,
        expire_time: None,
        ts_init: UnixNanos::new(1),
    })
}
fn names(events: &[OrderEvent]) -> Vec<&'static str> {
    events.iter().map(OrderEvent::name).collect()
}

// ---- Matching: forward trace validation ----------------------------------------------------------

struct Matching {
    sim: SimulatedExchange,
    ts: u64,
    qty: u64,
    post_only: bool,
    level: u64,
    bid_at_price: bool,
    submitted: bool,
    filled: u64,
    taker: u64,
}

impl Matching {
    const ACTIONS: [&'static str; 7] =
        ["Rest", "Take", "TradeAt", "Through", "Level", "Gone", "Cross"];

    fn new() -> Self {
        Self {
            sim: sim(FillModel::QueuePosition, StpMode::None),
            ts: 1,
            qty: 0,
            post_only: false,
            level: 0,
            bid_at_price: true,
            submitted: false,
            filled: 0,
            taker: 0,
        }
    }

    /// The market: the bid at p for `level` (or one tick below p after Gone) and the ask one tick
    /// above p, or at p for `offered` when set.
    fn quote(&mut self, offered: Option<u64>) {
        self.ts += 1;
        let (bid, bid_size) =
            if self.bid_at_price { (PRICE, self.level) } else { (PRICE - TICK, 1) };
        let (ask, ask_size) = match offered {
            Some(v) => (PRICE, v),
            None => (PRICE + TICK, 1),
        };
        self.data(&quote_at(bid, bid_size, ask, ask_size, self.ts));
    }
    fn trade(&mut self, price: i64, size: u64) {
        self.ts += 1;
        self.data(&trade_at(price, size, AggressorSide::Sell, self.ts));
    }
    fn data(&mut self, e: &Event) {
        self.sim.clear_events();
        self.sim.on_data(e, UnixNanos::new(self.ts)).unwrap();
        self.collect();
    }
    fn submit(&mut self) {
        self.ts += 1;
        self.sim.clear_events();
        let c = submit(1, OrderSide::Buy, self.qty, Some(PRICE), TimeInForce::Gtc, self.post_only);
        self.sim.on_command(&c, UnixNanos::new(self.ts)).unwrap();
        self.submitted = true;
        self.collect();
    }
    fn collect(&mut self) {
        for e in self.sim.events() {
            if let OrderEvent::Filled(f) = e {
                self.filled += f.last_qty.raw();
                if f.liquidity_side == LiquiditySide::Taker {
                    self.taker += f.last_qty.raw();
                }
            }
        }
    }
    fn compare(&self, s: &Step) -> Diff {
        let mut diff = Diff::default();
        let order = self.sim.open_orders().find(|o| o.client_order_id == cid(1)).copied();
        let phase = match (order, self.submitted) {
            (Some(_), _) => "RESTING",
            (None, true) => "DONE",
            (None, false) => "NEW",
        };
        diff.expect("phase", s.var("phase"), phase);
        diff.expect("filled", s.integer("filled"), self.filled as i64);
        diff.expect("taker", s.integer("taker"), self.taker as i64);
        diff.expect("level", s.integer("level"), self.level as i64);
        if let Some(o) = order {
            if phase == s.var("phase") {
                diff.expect("ahead", s.integer("ahead"), o.ahead_raw as i64);
            }
        }
        diff
    }
}

impl Replayer for Matching {
    fn actions(&self) -> &'static [&'static str] {
        &Self::ACTIONS
    }
    fn start(&mut self, init: &Step) -> Diff {
        self.qty = init.integer("qty") as u64;
        self.post_only = init.boolean("postOnly");
        self.level = init.integer("level") as u64;
        self.quote(None);
        self.compare(init)
    }
    fn step(&mut self, s: &Step) -> Diff {
        match s.action.as_str() {
            "Rest" => self.submit(),
            "Take" => {
                self.quote(Some(s.arg(0) as u64));
                self.submit();
                self.quote(None);
            }
            "TradeAt" => self.trade(PRICE, s.arg(0) as u64),
            "Through" => self.trade(PRICE - TICK, s.arg(0) as u64),
            "Level" => {
                self.level = s.arg(0) as u64;
                self.bid_at_price = true;
                self.quote(None);
            }
            "Gone" => {
                self.level = 0;
                self.bid_at_price = false;
                self.quote(None);
            }
            "Cross" => {
                self.quote(Some(s.arg(0) as u64));
                self.quote(None);
            }
            other => panic!("unknown Matching action {other}"),
        }
        self.compare(s)
    }
}

#[test]
fn matching_behaviours_replay() {
    let file = BehaviourFile::read(&behaviours_path(env!("CARGO_MANIFEST_DIR"), "Matching"));
    assert_eq!(file.spec, "Matching");
    replay_behaviours(&file, |_| Matching::new());
}

// ---- the venue's answers -------------------------------------------------------------------------

#[test]
#[allow(clippy::too_many_lines)] // one scenario per answer
fn venue_answers_like_binance() {
    let mut s = sim(FillModel::TopOfBook, StpMode::ExpireTaker);
    s.on_data(&quote_at(PRICE - TICK, 5, PRICE, 3, 2), UnixNanos::new(2)).unwrap();

    // A post-only order that would take is rejected with -5022.
    s.clear_events();
    s.on_command(
        &submit(1, OrderSide::Buy, 1, Some(PRICE), TimeInForce::Gtc, true),
        UnixNanos::new(3),
    )
    .unwrap();
    assert_eq!(names(s.events()), ["OrderRejected"]);
    let OrderEvent::Rejected(r) = s.events()[0] else { unreachable!() };
    assert!(r.due_post_only);
    assert_eq!(r.reason.as_str(), reject::POST_ONLY);

    // An IOC buy for 5 takes the 3 offered and expires the remainder; a fee is charged.
    s.clear_events();
    s.on_command(
        &submit(2, OrderSide::Buy, 5, Some(PRICE), TimeInForce::Ioc, false),
        UnixNanos::new(4),
    )
    .unwrap();
    assert_eq!(names(s.events()), ["OrderAccepted", "OrderFilled", "OrderExpired"]);
    let OrderEvent::Filled(f) = s.events()[1] else { unreachable!() };
    assert_eq!(f.last_qty, q9(3));
    assert_eq!(f.liquidity_side, LiquiditySide::Taker);
    assert_eq!(
        f.commission.unwrap().to_string(),
        "0.00000001 USDT",
        "3e-9 at 0.05% rounds up against the account"
    );
    assert_eq!(s.account().venue(data::InstrumentSlot(0)).unwrap().signed_raw(), 3);

    // FOK that cannot fill in full expires before any ACCEPTED.
    s.clear_events();
    s.on_command(
        &submit(3, OrderSide::Buy, 5, Some(PRICE), TimeInForce::Fok, false),
        UnixNanos::new(5),
    )
    .unwrap();
    assert_eq!(names(s.events()), ["OrderExpired"]);

    // A GTC that rests, then STP expires a crossing sell of ours (EXPIRE_TAKER).
    s.clear_events();
    s.on_command(
        &submit(4, OrderSide::Buy, 2, Some(PRICE - TICK), TimeInForce::Gtc, false),
        UnixNanos::new(6),
    )
    .unwrap();
    assert_eq!(names(s.events()), ["OrderAccepted"]);
    s.clear_events();
    s.on_command(
        &submit(5, OrderSide::Sell, 1, Some(PRICE - TICK), TimeInForce::Gtc, false),
        UnixNanos::new(7),
    )
    .unwrap();
    assert_eq!(names(s.events()), ["OrderAccepted", "OrderExpired"]);
    assert_eq!(s.open_orders().count(), 1);

    // Modify and cancel of unknown orders; a modify that keeps priority; a cancel.
    s.clear_events();
    let modify = |n: u64, qty: u64, price: i64| {
        Output::ModifyOrder(ModifyOrder {
            strategy_index: 0,
            client_order_id: cid(n),
            instrument_id: instrument_id(),
            venue_order_id: None,
            quantity: q9(qty),
            price: px(price),
            ts_init: UnixNanos::new(8),
        })
    };
    s.on_command(&modify(9, 1, PRICE), UnixNanos::new(8)).unwrap();
    let OrderEvent::ModifyRejected(m) = s.events()[0] else { unreachable!() };
    assert_eq!(m.reason.as_str(), reject::ORDER_MISSING);
    s.clear_events();
    s.on_command(&modify(4, 3, PRICE - TICK), UnixNanos::new(9)).unwrap();
    assert_eq!(names(s.events()), ["OrderUpdated"]);
    s.clear_events();
    let cancel = |n: u64| {
        Output::CancelOrder(CancelOrder {
            strategy_index: 0,
            client_order_id: cid(n),
            instrument_id: instrument_id(),
            venue_order_id: None,
            ts_init: UnixNanos::new(10),
        })
    };
    s.on_command(&cancel(9), UnixNanos::new(10)).unwrap();
    let OrderEvent::CancelRejected(c) = s.events()[0] else { unreachable!() };
    assert_eq!(c.reason.as_str(), reject::UNKNOWN_ORDER);
    s.clear_events();
    s.on_command(&cancel(4), UnixNanos::new(11)).unwrap();
    assert_eq!(names(s.events()), ["OrderCanceled"]);
    assert_eq!(s.open_orders().count(), 0);

    // A market order with no market, a GTD order expiring at its time.
    s.clear_events();
    let mut fresh = sim(FillModel::TopOfBook, StpMode::None);
    fresh
        .on_command(
            &submit(6, OrderSide::Buy, 1, None, TimeInForce::Ioc, false),
            UnixNanos::new(12),
        )
        .unwrap();
    let OrderEvent::Rejected(r) = fresh.events()[0] else { unreachable!() };
    assert_eq!(r.reason.as_str(), reject::NO_MARKET);
    fresh.clear_events();
    fresh.on_data(&quote_at(PRICE - TICK, 5, PRICE + TICK, 3, 13), UnixNanos::new(13)).unwrap();
    let mut gtd = submit(7, OrderSide::Buy, 1, Some(PRICE), TimeInForce::Gtd, false);
    if let Output::SubmitOrder(c) = &mut gtd {
        c.expire_time = Some(UnixNanos::new(20));
    }
    fresh.on_command(&gtd, UnixNanos::new(14)).unwrap();
    assert_eq!(names(fresh.events()), ["OrderAccepted"]);
    fresh.clear_events();
    fresh.on_data(&quote_at(PRICE - TICK, 5, PRICE + TICK, 3, 20), UnixNanos::new(20)).unwrap();
    assert_eq!(names(fresh.events()), ["OrderExpired"]);
    assert_eq!(s.stats().rejected, 1);
}

// ---- sources and lifecycle ---------------------------------------------------------------------

#[test]
fn merge_source_orders_by_key_and_refuses_regressions() {
    let a = VecSource::new(vec![
        (EventKey::new(UnixNanos::new(1), 1, 1), trade_at(PRICE, 1, AggressorSide::Buy, 1)),
        (EventKey::new(UnixNanos::new(3), 1, 2), trade_at(PRICE, 1, AggressorSide::Buy, 3)),
    ]);
    let b = VecSource::new(vec![
        (EventKey::new(UnixNanos::new(2), 2, 1), trade_at(PRICE, 1, AggressorSide::Buy, 2)),
        (EventKey::new(UnixNanos::new(3), 2, 2), trade_at(PRICE, 1, AggressorSide::Buy, 3)),
    ]);
    let mut m = MergeSource::new(vec![Box::new(a), Box::new(b)]);
    let mut keys = Vec::new();
    while let Some((k, _)) = m.next().unwrap() {
        keys.push((k.ts.value(), k.source_id));
    }
    assert_eq!(keys, vec![(1, 1), (2, 2), (3, 1), (3, 2)]);

    let bad = VecSource::new(vec![
        (EventKey::new(UnixNanos::new(2), 1, 1), trade_at(PRICE, 1, AggressorSide::Buy, 2)),
        (EventKey::new(UnixNanos::new(1), 1, 2), trade_at(PRICE, 1, AggressorSide::Buy, 1)),
    ]);
    let mut m = MergeSource::new(vec![Box::new(bad)]);
    assert!(m.next().unwrap().is_some());
    assert_eq!(m.next().err(), Some(kernel_core::Status::InvalidArgument));
}

#[test]
fn lifecycle_table() {
    use LifecycleReason as R;
    use NodeState as S;
    let mut l = Lifecycle::default();
    for (reason, to) in [
        (R::Configured, S::Wired),
        (R::RunRequested, S::Starting),
        (R::Started, S::Syncing),
        (R::Synced, S::Running),
        (R::HealthLost, S::Degraded),
        (R::HealthRestored, S::Syncing),
        (R::Synced, S::Running),
        (R::EndOfData, S::Stopping),
        (R::ShutdownRequested, S::Stopping),
        (R::Drained, S::Stopped),
    ] {
        let e = l.apply(reason, UnixNanos::new(1)).unwrap();
        assert_eq!((e.reason, e.to), (reason, to));
    }
    assert_eq!(
        next_state(S::Stopped, R::Fault).err(),
        Some(kernel_core::Status::InvalidTransition)
    );
    assert_eq!(next_state(S::Running, R::Fault), Ok(S::Faulted));
    assert_eq!(next_state(S::Init, R::Synced).err(), Some(kernel_core::Status::InvalidTransition));
}

// ---- a whole run ---------------------------------------------------------------------------------

/// Buys at the bid on every third quote while it has no open order; records what it sees.
struct Dipper {
    quotes: u32,
    fills: Rc<Cell<u32>>,
    first_fill_lag: Rc<Cell<Option<u64>>>,
    last_quote_init: UnixNanos,
}

impl Strategy for Dipper {
    fn on_start(&mut self, ctx: &mut Context<'_>) -> Result<()> {
        ctx.subscribe_quotes(&instrument_id(), Cadence::Every)
    }
    fn on_quote(&mut self, ctx: &mut Context<'_>, q: &QuoteTick) -> Result<()> {
        self.quotes += 1;
        self.last_quote_init = q.ts_init;
        assert!(q.ts_init > q.ts_event, "the strategy sees the venue's past");
        let mut open = [strategy::OrderView {
            client_order_id: ClientOrderId::default(),
            venue_order_id: None,
            instrument_id: instrument_id(),
            side: OrderSide::Buy,
            order_type: OrderType::Limit,
            time_in_force: TimeInForce::Gtc,
            post_only: false,
            reduce_only: false,
            price: None,
            status: model::enums::OrderStatus::Initialized,
            quantity: Quantity::default(),
            filled: Quantity::default(),
            leaves: Quantity::default(),
            avg_px: None,
            ts_init: UnixNanos::default(),
        }; 4];
        if self.quotes % 3 == 0 && ctx.open_orders(None, &mut open) == 0 {
            let intent = OrderIntent::limit(
                instrument_id(),
                OrderSide::Buy,
                Quantity::parse("0.010").unwrap(),
                q.bid_price,
                TimeInForce::Gtc,
            );
            ctx.submit(&intent)?;
            ctx.record("quotes", i64::from(self.quotes) * model::FIXED_SCALAR)?;
        }
        Ok(())
    }
    fn on_order_event(&mut self, ctx: &mut Context<'_>, e: &OrderEvent) -> Result<()> {
        if let OrderEvent::Filled(f) = e {
            self.fills.set(self.fills.get() + 1);
            if self.first_fill_lag.get().is_none() {
                self.first_fill_lag.set(Some(ctx.now().value() - f.header.ts_event.value()));
            }
        }
        Ok(())
    }
}

fn data(n: u64) -> Vec<(EventKey, Event)> {
    let base = 1_700_000_000_000_000_000u64;
    let mut out = Vec::new();
    for i in 0..n {
        let ts = base + i * 10_000_000; // 10 ms apart
        let bid = PRICE - TICK * (i as i64 % 7);
        out.push((
            EventKey::new(UnixNanos::new(ts), 1, i * 2 + 1),
            quote_at(bid, 5, bid + TICK, 5, ts),
        ));
        out.push((
            EventKey::new(UnixNanos::new(ts + 5_000_000), 1, i * 2 + 2),
            trade_at(bid - TICK, 1_000_000, AggressorSide::Sell, ts + 5_000_000),
        ));
    }
    out
}

fn config() -> KernelConfig {
    let mut c = KernelConfig { instruments: 4, strategies: 1, ..KernelConfig::default() };
    c.trading.orders = 64;
    c.trading.trades = 256;
    c.book_window_levels = 256;
    c.book_overflow_levels = 64;
    c
}

fn engine(fills: &Rc<Cell<u32>>, lag: &Rc<Cell<Option<u64>>>) -> Engine {
    let dipper = Dipper {
        quotes: 0,
        fills: Rc::clone(fills),
        first_fill_lag: Rc::clone(lag),
        last_quote_init: UnixNanos::default(),
    };
    Engine::new(config(), vec![Box::new(dipper)], ErrorPolicy::HaltStrategy).unwrap()
}

fn venue_config(seed: u64) -> VenueLoopConfig {
    VenueLoopConfig {
        sim: SimConfig {
            instruments: 4,
            orders: 64,
            fees: cost::MakerTakerFees::schedule("binance_usdm_vip0").unwrap(),
            seed,
            ..SimConfig::default()
        },
        feed: DurationNanos::from_millis(1),
        outbound: DurationNanos::from_millis(2),
        inbound: DurationNanos::from_millis(3),
        jitter: DurationNanos::from_millis(1),
        seed,
        ..VenueLoopConfig::default()
    }
}

fn run(seed: u64) -> (Vec<u8>, String, backtest::RunSummary, u32, Option<u64>) {
    let fills = Rc::new(Cell::new(0));
    let lag = Rc::new(Cell::new(None));
    let mut e = engine(&fills, &lag);
    let header = LogHeader::new([1u8; 32], seed, "bt", "backtest").unwrap();
    let mut recorder = LogRecorder::new(model::log::LogWriter::new(&header));
    let source =
        Source::Venue(VenueLoop::new(venue_config(seed), Box::new(VecSource::new(data(60)))));
    let options = DriverOptions {
        preamble: vec![
            Event::Instrument(perpetual(3)),
            Event::AccountState(account(100_000, UnixNanos::new(1))),
        ],
        ..DriverOptions::default()
    };
    let summary = Driver::new(&mut e, source, &mut recorder, options).run().unwrap();
    let (log, fp) = recorder.finish();
    (log, fp.to_string(), summary, fills.get(), lag.get())
}

#[test]
fn a_run_through_the_venue_loop_replays_byte_for_byte() {
    let (log, fp, summary, fills, lag) = run(7);
    assert_eq!(summary.state, NodeState::Stopped);
    assert_eq!(summary.data_events, 120);
    assert!(summary.venue_answers >= 3, "answers: {}", summary.venue_answers);
    assert!(fills >= 1, "the dipper got filled at least once");
    assert!(lag.unwrap() >= 3_000_000, "a fill reaches the strategy after the inbound latency");
    assert!(summary.inputs > summary.data_events + summary.venue_answers + summary.batches);
    assert_eq!(summary.left_open + fills, summary.left_open + fills); // placeholder sanity

    // The same run again is the same log.
    let (again, fp2, _, _, _) = run(7);
    assert_eq!(fp, fp2);
    assert_eq!(log, again);
    // Another seed draws other latencies: a different log.
    let (_, fp3, _, _, _) = run(8);
    assert_ne!(fp, fp3);

    // A fresh engine replays the log's inputs and reproduces every output.
    let fills = Rc::new(Cell::new(0));
    let lag = Rc::new(Cell::new(None));
    let mut e = engine(&fills, &lag);
    let report = replay(&log, &mut e).unwrap_or_else(|e| panic!("{e:?}"));
    assert_eq!(report.inputs, summary.inputs);
    assert_eq!(report.outputs, summary.outputs);
    assert_eq!(fills.get(), summary.venue_answers.min(u64::from(fills.get())) as u32);

    // The log's inputs alone, through the plain driver with a fingerprint recorder, are the run.
    let mut plain = engine(&Rc::new(Cell::new(0)), &Rc::new(Cell::new(None)));
    let mut inputs = Vec::new();
    let mut source = ReplaySource::open(&log).unwrap();
    while let Some(item) = source.next().unwrap() {
        inputs.push(item);
    }
    assert_eq!(inputs.len() as u64, summary.inputs);
    let mut fp_rec = FingerprintRecorder::default();
    for (k, ev) in &inputs {
        plain.step(*k, ev).unwrap();
        fp_rec.record(*k, ev).unwrap();
        for (i, o) in plain.outputs().iter().enumerate() {
            fp_rec.emit(EventKey::new(k.ts, i as u16, k.seq), o).unwrap();
        }
    }
    assert_eq!(fp_rec.finish().to_string(), fp);

    // A tampered log diverges.
    let mut e = engine(&Rc::new(Cell::new(0)), &Rc::new(Cell::new(None)));
    let mut altered = KernelConfig::default();
    altered.trading.risk.initial_state = model::enums::TradingState::Halted;
    let _ = &mut altered;
    let mut halted = Engine::new(
        {
            let mut c = config();
            c.trading.risk.initial_state = model::enums::TradingState::Halted;
            c
        },
        vec![Box::new(Dipper {
            quotes: 0,
            fills: Rc::new(Cell::new(0)),
            first_fill_lag: Rc::new(Cell::new(None)),
            last_quote_init: UnixNanos::default(),
        })],
        ErrorPolicy::HaltStrategy,
    )
    .unwrap();
    assert!(matches!(replay(&log, &mut halted), Err(ReplayError::Divergence(_))));
    let _ = &mut e;
}
