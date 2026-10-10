//! The engine end to end with a small strategy: subscriptions and cadences, the command chain
//! (intent -> gates -> OMS -> `SubmitOrder` output -> kernel events), the venue return path (OMS
//! -> portfolio -> strategy), timers, bars, the error policy, and snapshots that continue
//! identically.

use std::cell::RefCell;
use std::rc::Rc;

use corpus::fixtures::{account, header, order_event, perpetual, Fill};
use data::subscription::{Cadence, StrategyIndex};
use data::FeatureKind;
use engine::Engine;
use execution::OrderIntent;
use kernel_core::clock::TimerKey;
use kernel_core::{EventKey, Result, Status, UnixNanos};
use model::bar::{BarSpecification, BarType};
use model::data::{QuoteTick, TradeTick};
use model::enums::{
    AggregationSource, AggressorSide, BarAggregation, LifecycleReason, NodeState, OrderSide,
    OrderStatus, PriceType, TimeInForce,
};
use model::event::{BatchEnd, NodeLifecycle, TimerFired};
use model::order_events::OrderEvent;
use model::outputs::Output;
use model::position_events::PositionEvent;
use model::{ClientOrderId, Event, InstrumentId, Price, Quantity, TradeId};
use strategy::{Context, ErrorPolicy, KernelConfig, Strategy};

fn iid() -> InstrumentId {
    corpus::fixtures::instrument_id()
}
fn ts(n: u64) -> UnixNanos {
    UnixNanos::new(1_700_000_000_000_000_000 + n * 1_000_000)
}
fn key(n: u64) -> EventKey {
    EventKey::new(ts(n), 1, n)
}
fn trade(n: u64, px: &str, size: &str) -> Event {
    Event::TradeTick(
        TradeTick::new(
            iid(),
            Price::parse(px).unwrap(),
            Quantity::parse(size).unwrap(),
            AggressorSide::Buy,
            TradeId::new(&format!("T{n}")).unwrap(),
            ts(n),
            ts(n),
        )
        .unwrap(),
    )
}
fn quote(n: u64, bid: &str, ask: &str) -> Event {
    Event::QuoteTick(
        QuoteTick::new(
            iid(),
            Price::parse(bid).unwrap(),
            Price::parse(ask).unwrap(),
            Quantity::parse("1.000").unwrap(),
            Quantity::parse("1.000").unwrap(),
            ts(n),
            ts(n),
        )
        .unwrap(),
    )
}
fn lifecycle(n: u64, from: NodeState, to: NodeState) -> Event {
    Event::NodeLifecycle(NodeLifecycle { from, to, reason: LifecycleReason::Synced, ts: ts(n) })
}

/// What the strategy saw, shared with the test.
#[derive(Default, Debug)]
struct Seen {
    trades: Vec<u64>,
    quotes: u32,
    books: u32,
    bars: Vec<Price>,
    features: u32,
    orders: Vec<OrderStatus>,
    positions: Vec<&'static str>,
    timers: Vec<u32>,
    started: bool,
    stopped: bool,
    batch_trades: Vec<usize>,
    order_id: Option<ClientOrderId>,
    position_after_fill: Option<i64>,
}

struct Taker {
    seen: Rc<RefCell<Seen>>,
    cadence: Cadence,
    fail_on_trade: Option<u64>,
    count: u32,
}

impl Strategy for Taker {
    fn on_start(&mut self, ctx: &mut Context<'_>) -> Result<()> {
        self.seen.borrow_mut().started = true;
        ctx.subscribe_trades(&iid(), self.cadence)?;
        ctx.subscribe_quotes(&iid(), Cadence::sampled_ms(5)?)?;
        ctx.subscribe_book(&iid(), Cadence::Every)?;
        ctx.subscribe_bars(
            &BarType {
                instrument_id: iid(),
                spec: BarSpecification {
                    step: 2,
                    aggregation: BarAggregation::Tick,
                    price_type: PriceType::Last,
                },
                aggregation_source: AggregationSource::Internal,
                composite: None,
            },
            Cadence::Every,
        )?;
        ctx.feature(FeatureKind::Ema { period: 2 }, &iid(), Cadence::Every)?;
        ctx.set_timer(7, ctx.now().plus(kernel_core::DurationNanos::from_millis(2))?)?;
        Ok(())
    }
    fn on_stop(&mut self, _: &mut Context<'_>) -> Result<()> {
        self.seen.borrow_mut().stopped = true;
        Ok(())
    }
    fn on_trade(&mut self, ctx: &mut Context<'_>, t: &TradeTick) -> Result<()> {
        if self.fail_on_trade == Some(t.ts_event.value()) {
            return Err(Status::InvalidState);
        }
        self.seen.borrow_mut().trades.push(t.ts_event.value());
        self.count += 1;
        if self.count == 1 {
            let intent = OrderIntent::limit(
                iid(),
                OrderSide::Buy,
                Quantity::parse("0.500").unwrap(),
                Price::parse("99.0").unwrap(),
                TimeInForce::Gtc,
            );
            let id = ctx.submit(&intent)?;
            self.seen.borrow_mut().order_id = Some(id);
        }
        Ok(())
    }
    fn on_trade_batch(
        &mut self,
        _: &mut Context<'_>,
        _: &InstrumentId,
        trades: &[TradeTick],
    ) -> Result<()> {
        self.seen.borrow_mut().batch_trades.push(trades.len());
        Ok(())
    }
    fn on_quote(&mut self, _: &mut Context<'_>, _: &QuoteTick) -> Result<()> {
        self.seen.borrow_mut().quotes += 1;
        Ok(())
    }
    fn on_book(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        book: &data::BookView<'_>,
    ) -> Result<()> {
        assert!(ctx.book(id).is_none(), "the book is on loan during on_book");
        assert!(book.best_bid().is_some());
        self.seen.borrow_mut().books += 1;
        Ok(())
    }
    fn on_bar(&mut self, _: &mut Context<'_>, bar: &model::bar::Bar) -> Result<()> {
        self.seen.borrow_mut().bars.push(bar.close);
        Ok(())
    }
    fn on_feature(
        &mut self,
        _: &mut Context<'_>,
        _: data::FeatureId,
        _: data::FeatureValue,
        _: UnixNanos,
        _: UnixNanos,
    ) -> Result<()> {
        self.seen.borrow_mut().features += 1;
        Ok(())
    }
    fn on_order_event(&mut self, ctx: &mut Context<'_>, e: &OrderEvent) -> Result<()> {
        let view = ctx.order(&e.header().client_order_id).expect("own order");
        self.seen.borrow_mut().orders.push(view.status);
        if matches!(e, OrderEvent::Filled(_)) {
            self.seen.borrow_mut().position_after_fill = ctx.position(&iid()).map(|p| p.signed_raw);
        }
        Ok(())
    }
    fn on_position_event(&mut self, _: &mut Context<'_>, e: &PositionEvent) -> Result<()> {
        self.seen.borrow_mut().positions.push(match e {
            PositionEvent::Opened(_) => "opened",
            PositionEvent::Changed(_) => "changed",
            PositionEvent::Closed(_) => "closed",
            PositionEvent::Adjusted(_) => "adjusted",
        });
        Ok(())
    }
    fn on_timer(&mut self, _: &mut Context<'_>, key: TimerKey, _: UnixNanos) -> Result<()> {
        self.seen.borrow_mut().timers.push(key.id);
        Ok(())
    }
    fn has_state(&self) -> bool {
        true
    }
    fn save_state(&self, w: &mut kernel_core::state::StateWriter<'_>) {
        w.u32(self.count);
    }
    fn load_state(&mut self, r: &mut kernel_core::state::StateReader<'_>) {
        self.count = r.u32();
    }
}

fn config() -> KernelConfig {
    let mut c = KernelConfig {
        instruments: 4,
        strategies: 2,
        timers: 16,
        batch: 8,
        ..KernelConfig::default()
    };
    c.trading.orders = 16;
    c.trading.trades = 32;
    c.trading.risk.check_margin = true;
    c.book_window_levels = 256;
    c.book_overflow_levels = 64;
    c
}

fn engine(cadence: Cadence, fail_on_trade: Option<u64>) -> (Engine, Rc<RefCell<Seen>>) {
    let seen = Rc::new(RefCell::new(Seen::default()));
    let taker = Taker { seen: Rc::clone(&seen), cadence, fail_on_trade, count: 0 };
    let engine = Engine::new(config(), vec![Box::new(taker)], ErrorPolicy::HaltStrategy).unwrap();
    (engine, seen)
}

/// Instrument, account, start.
fn preamble(e: &mut Engine) {
    e.step(key(1), &Event::Instrument(perpetual(3))).unwrap();
    e.step(key(2), &Event::AccountState(account(10_000, ts(2)))).unwrap();
    e.step(key(3), &lifecycle(3, NodeState::Syncing, NodeState::Running)).unwrap();
}

/// Feeds the venue's answer to the strategy's order and returns the engine's outputs.
fn venue(e: &mut Engine, n: u64, tag: u8, fill: &Fill, cid: ClientOrderId) {
    let ev = order_event(tag, header(cid, ts(n)), fill);
    e.step(key(n), &Event::Order(ev)).unwrap();
}

#[test]
fn trade_to_strategy_to_order_to_fill() {
    let (mut e, seen) = engine(Cadence::Every, None);
    preamble(&mut e);
    assert!(seen.borrow().started);
    assert_eq!(e.kernel().trading.risk.trading_state(), model::enums::TradingState::Active);

    e.step(key(10), &quote(10, "99.0", "99.1")).unwrap();
    assert_eq!(seen.borrow().quotes, 1);
    assert_eq!(seen.borrow().books, 1, "a quote feeds the L1 book");
    e.step(key(11), &trade(11, "99.1", "0.100")).unwrap();
    let outputs: Vec<Output> = e.outputs().to_vec();
    let submit = outputs.iter().find_map(|o| match o {
        Output::SubmitOrder(s) => Some(*s),
        _ => None,
    });
    let submit = submit.expect("a SubmitOrder output");
    assert_eq!(submit.quantity, Quantity::parse("0.500").unwrap());
    let cid = seen.borrow().order_id.unwrap();
    assert_eq!(submit.client_order_id, cid);
    assert_eq!(cid.as_str(), "jarvis-AAAAAB-AAAAAAAB");
    assert_eq!(
        seen.borrow().orders,
        vec![OrderStatus::Submitted],
        "OrderSubmitted after the callback"
    );
    assert_eq!(e.open_orders(), 1);
    assert_eq!(
        e.kernel()
            .trading
            .exposure(data::InstrumentSlot(0), &e.kernel().instruments)
            .unwrap()
            .open_buy,
        Quantity::parse("0.500000000").unwrap()
    );

    let fill = Fill::new("V1", Quantity::parse("0.500").unwrap(), Price::parse("99.0").unwrap());
    venue(&mut e, 12, 6, &fill, cid);
    assert_eq!(seen.borrow().orders.last(), Some(&OrderStatus::Accepted));
    venue(&mut e, 13, 16, &fill, cid);
    assert_eq!(seen.borrow().orders.last(), Some(&OrderStatus::Filled));
    assert_eq!(
        seen.borrow().position_after_fill,
        Some(500_000_000),
        "the fill is booked before the callback"
    );
    assert_eq!(seen.borrow().positions, vec!["opened"]);
    assert_eq!(e.open_orders(), 0);
    // The same trade again is a duplicate: counted, not delivered.
    venue(&mut e, 14, 16, &fill, cid);
    assert_eq!(e.kernel().trading.stats.duplicate_fills, 1);
    assert_eq!(seen.borrow().orders.len(), 3);
    // An event for an order the OMS never saw.
    venue(&mut e, 15, 6, &fill, ClientOrderId::new("stranger").unwrap());
    assert_eq!(e.kernel().trading.stats.unknown_order_events, 1);

    // Two trades close a 2-tick bar; the EMA produced a value per trade.
    e.step(key(16), &trade(16, "99.2", "0.100")).unwrap();
    assert_eq!(seen.borrow().bars, vec![Price::parse("99.2").unwrap()]);
    assert_eq!(seen.borrow().features, 2);
    assert!(e.outputs().iter().any(|o| matches!(o, Output::FeatureUpdate(_))));

    // The timer set in on_start comes due.
    let next = e.next_timer().expect("a timer");
    assert_eq!(next.key, TimerKey::new(0, 7));
    e.step(key(17), &Event::TimerFired(TimerFired { key: next.key, deadline: next.deadline }))
        .unwrap();
    assert_eq!(seen.borrow().timers, vec![7]);
    assert!(e.next_timer().is_none());

    e.step(key(18), &lifecycle(18, NodeState::Running, NodeState::Stopping)).unwrap();
    assert!(seen.borrow().stopped);
    e.step(key(19), &trade(19, "99.3", "0.100")).unwrap();
    assert_eq!(seen.borrow().trades.len(), 2, "no callbacks after on_stop");
    assert!(e.kernel().trading.portfolio.ledger_is_consistent());
}

#[test]
fn conflated_and_batch_cadences_deliver_at_batch_end() {
    let (mut e, seen) = engine(Cadence::Conflated, None);
    preamble(&mut e);
    e.step(key(10), &trade(10, "99.1", "0.100")).unwrap();
    e.step(key(10), &trade(10, "99.2", "0.100")).unwrap();
    assert!(seen.borrow().trades.is_empty());
    e.step(key(10), &Event::BatchEnd(BatchEnd { ts: ts(10) })).unwrap();
    assert_eq!(seen.borrow().trades, vec![ts(10).value()], "the latest update, once");
    assert_eq!(seen.borrow().orders, vec![OrderStatus::Submitted]);

    let (mut e, seen) = engine(Cadence::OnBatch, None);
    preamble(&mut e);
    for n in 0..10 {
        e.step(key(10), &trade(10 + n, "99.1", "0.100")).unwrap();
    }
    assert_eq!(seen.borrow().batch_trades, vec![8], "a full buffer is delivered early");
    e.step(key(20), &Event::BatchEnd(BatchEnd { ts: ts(20) })).unwrap();
    assert_eq!(seen.borrow().batch_trades, vec![8, 2]);
    // Sampled quotes: one per 5 ms period.
    for n in 0..10 {
        e.step(key(30 + n), &quote(30 + n, "99.0", "99.1")).unwrap();
    }
    assert_eq!(seen.borrow().quotes, 2);
}

#[test]
fn a_failing_callback_halts_the_strategy_and_cancels_its_orders() {
    let (mut e, seen) = engine(Cadence::Every, Some(ts(12).value()));
    preamble(&mut e);
    e.step(key(11), &trade(11, "99.1", "0.100")).unwrap();
    let cid = seen.borrow().order_id.unwrap();
    let fill = Fill::new("V1", Quantity::parse("0.500").unwrap(), Price::parse("99.0").unwrap());
    venue(&mut e, 11, 6, &fill, cid);
    e.step(key(12), &trade(12, "99.1", "0.100")).unwrap();
    assert_eq!(
        e.failures(),
        &[strategy::StrategyFailure { strategy: StrategyIndex(0), status: Status::InvalidState }]
    );
    assert!(e
        .outputs()
        .iter()
        .any(|o| matches!(o, Output::CancelOrder(c) if c.client_order_id == cid)));
    assert!(e.kernel().is_disabled(StrategyIndex(0)));
    e.step(key(13), &trade(13, "99.1", "0.100")).unwrap();
    assert_eq!(seen.borrow().trades, vec![ts(11).value()]);
}

#[test]
fn denied_orders_are_events_and_outputs() {
    let mut c = config();
    c.trading.risk.initial_state = model::enums::TradingState::Reducing;
    let seen = Rc::new(RefCell::new(Seen::default()));
    let taker =
        Taker { seen: Rc::clone(&seen), cadence: Cadence::Every, fail_on_trade: None, count: 0 };
    let mut e = Engine::new(c, vec![Box::new(taker)], ErrorPolicy::HaltStrategy).unwrap();
    preamble(&mut e);
    e.step(key(11), &trade(11, "99.1", "0.100")).unwrap();
    assert_eq!(seen.borrow().orders, vec![OrderStatus::Denied]);
    let denied = e.outputs().iter().find_map(|o| match o {
        Output::OrderDenied(d) => Some(*d),
        _ => None,
    });
    assert_eq!(denied.unwrap().reason.as_str(), "TRADING_REDUCING_ONLY");
    assert!(!e.outputs().iter().any(|o| matches!(o, Output::SubmitOrder(_))));
    assert_eq!(e.kernel().trading.stats.denied, 1);
}

#[test]
fn snapshot_continues_identically() {
    let (mut a, _) = engine(Cadence::Every, None);
    preamble(&mut a);
    a.step(key(10), &quote(10, "99.0", "99.1")).unwrap();
    a.step(key(11), &trade(11, "99.1", "0.100")).unwrap();
    let cid = a.kernel().trading.oms.iter().next().unwrap().1.client_order_id;
    let bytes = a.save_state().unwrap();
    assert!(a.snapshot_complete());

    let (mut b, _) = engine(Cadence::Every, None);
    b.load_state(&bytes).unwrap();
    assert_eq!(b.save_state().unwrap(), bytes, "a restored snapshot saves the same bytes");

    let fill = Fill::new("V1", Quantity::parse("0.500").unwrap(), Price::parse("99.0").unwrap());
    let mut script: Vec<(EventKey, Event)> = vec![
        (key(12), Event::Order(order_event(6, header(cid, ts(12)), &fill))),
        (key(13), Event::Order(order_event(16, header(cid, ts(13)), &fill))),
        (key(14), trade(14, "99.3", "0.200")),
        (key(15), trade(15, "99.4", "0.200")),
    ];
    let next = a.next_timer().unwrap();
    script
        .push((key(16), Event::TimerFired(TimerFired { key: next.key, deadline: next.deadline })));
    for (k, ev) in &script {
        a.step(*k, ev).unwrap();
        b.step(*k, ev).unwrap();
        assert_eq!(a.outputs(), b.outputs(), "outputs of {ev:?}");
        assert_eq!(a.save_state().unwrap(), b.save_state().unwrap(), "state after {ev:?}");
    }
    assert_eq!(
        a.kernel()
            .trading
            .portfolio
            .position(StrategyIndex(0), data::InstrumentSlot(0))
            .unwrap()
            .signed_raw(),
        500_000_000
    );

    let (mut wrong, _) = engine(Cadence::Every, None);
    let mut truncated = bytes.clone();
    truncated.pop();
    assert!(wrong.load_state(&truncated).is_err());
}
