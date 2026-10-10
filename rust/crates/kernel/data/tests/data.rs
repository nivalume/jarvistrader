//! The data layer: interning, subscriptions and cadences, routing, the order book, bar
//! aggregation and features. Unit cases, properties against simple reference models, and a golden
//! fingerprint of everything the layer derives from the seed 7 corpus.

use corpus::Corpus;
use data::bars::BarAggregator;
use data::book::{BookConfig, BookLevel, OrderBook, Side};
use data::features::{FeatureGraph, FeatureKind, FeatureSpec, FeatureValue};
use data::intern::{BarTable, InstrumentSlot, InstrumentTable, InternTable};
use data::router::route_of;
use data::subscription::{
    Cadence, DataKind, Decision, StrategyIndex, Subscriber, SubscriptionMatrix,
};
use kernel_core::sha256::Sha256;
use kernel_core::{load_state, save_state, Status, UnixNanos};
use model::bar::BarType;
use model::data::{BookOrder, OrderBookDelta, QuoteTick, TradeTick};
use model::enums::{AggressorSide, BookAction, OrderSide};
use model::{Event, InstrumentId, Price, Quantity, TradeId, Wire};
use testkit::{for_all, Gen};

fn id(s: &str) -> InstrumentId {
    InstrumentId::parse(s).unwrap()
}
fn price(s: &str) -> Price {
    Price::parse(s).unwrap()
}
fn qty(s: &str) -> Quantity {
    Quantity::parse(s).unwrap()
}
fn ts(n: u64) -> UnixNanos {
    UnixNanos::new(n)
}
fn trade(p: &str, q: &str, t: u64) -> TradeTick {
    TradeTick::new(
        id("BTCUSDT-PERP.BINANCE"),
        price(p),
        qty(q),
        AggressorSide::Buy,
        TradeId::new("1").unwrap(),
        ts(t),
        ts(t),
    )
    .unwrap()
}
fn quote(bid: &str, ask: &str, bs: &str, as_: &str, t: u64) -> QuoteTick {
    QuoteTick::new(
        id("BTCUSDT-PERP.BINANCE"),
        price(bid),
        price(ask),
        qty(bs),
        qty(as_),
        ts(t),
        ts(t),
    )
    .unwrap()
}
fn delta(action: BookAction, side: OrderSide, p: &str, q: &str, seq: u64) -> OrderBookDelta {
    let order = BookOrder { side: Some(side), price: price(p), size: qty(q), order_id: 0 };
    OrderBookDelta::new(id("BTCUSDT-PERP.BINANCE"), action, order, 0, seq, ts(seq), ts(seq))
        .unwrap()
}
fn book() -> OrderBook {
    OrderBook::new(BookConfig {
        price_increment: price("0.1"),
        size_precision: 3,
        window_levels: 64,
        overflow_levels: 32,
    })
    .unwrap()
}
fn levels(book: &OrderBook, side: Side) -> Vec<(String, String)> {
    let mut out = [BookLevel::default(); 64];
    let n = book.levels(side, &mut out);
    out[..n].iter().map(|l| (l.price.to_string(), l.size.to_string())).collect()
}

// ---- interning ----------------------------------------------------------------------------------

#[test]
fn interning_assigns_slots_in_first_seen_order() {
    let mut table: InternTable<u32> = InternTable::with_capacity(2);
    assert_eq!(table.intern(7), Ok(0));
    assert_eq!(table.intern(9), Ok(1));
    assert_eq!(table.intern(7), Ok(0));
    assert_eq!(table.intern(11), Err(Status::CapacityExceeded));
    assert_eq!(table.slot_of(&9), Some(1));
    assert_eq!(table.key_of(2), None);

    let mut instruments = InstrumentTable::with_capacity(4);
    let slot = instruments.intern(id("BTCUSDT-PERP.BINANCE")).unwrap();
    assert_eq!(slot, InstrumentSlot(0));
    assert_eq!(instruments.require(slot), Err(Status::NotFound));
    assert_eq!(
        instruments.id_of(slot).map(ToString::to_string),
        Some("BTCUSDT-PERP.BINANCE".into())
    );
}

// ---- subscriptions ------------------------------------------------------------------------------

#[test]
fn cadences_decide_as_documented() {
    let mut every = Subscriber::new(StrategyIndex(0), Cadence::Every);
    assert_eq!(every.on_update(ts(5)), Decision::Now);
    assert_eq!(every.on_update(ts(5)), Decision::Now);

    let mut sampled = Subscriber::new(StrategyIndex(0), Cadence::sampled_ms(1).unwrap());
    assert_eq!(sampled.on_update(ts(1_500_000)), Decision::Now, "first in period 1");
    assert_eq!(sampled.on_update(ts(1_900_000)), Decision::Skip, "same period");
    assert_eq!(
        sampled.on_update(ts(2_000_000)),
        Decision::Now,
        "period 2 starts exactly at the epoch-aligned boundary"
    );
    assert_eq!(
        sampled.on_update(ts(1_999_999)),
        Decision::Now,
        "a late update of period 1 is a new period for the sampler"
    );

    let mut conflated = Subscriber::new(StrategyIndex(0), Cadence::Conflated);
    assert!(!conflated.on_batch_end());
    assert_eq!(conflated.on_update(ts(1)), Decision::Defer);
    assert_eq!(conflated.on_update(ts(2)), Decision::Defer);
    assert!(conflated.on_batch_end());
    assert!(!conflated.on_batch_end());
    assert_eq!(Cadence::sampled_ns(0), Err(Status::InvalidArgument));
}

#[test]
fn matrix_keeps_subscription_order_and_round_trips() {
    let mut m = SubscriptionMatrix::new(4, 3);
    m.subscribe(1, DataKind::Trade, StrategyIndex(2), Cadence::Every).unwrap();
    m.subscribe(1, DataKind::Trade, StrategyIndex(0), Cadence::Conflated).unwrap();
    m.subscribe(1, DataKind::Trade, StrategyIndex(1), Cadence::OnBatch).unwrap();
    assert_eq!(
        m.subscribe(1, DataKind::Trade, StrategyIndex(2), Cadence::sampled_ms(5).unwrap()),
        Ok(()),
        "re-subscribing changes the cadence"
    );
    assert_eq!(
        m.subscribers(1, DataKind::Trade).iter().map(|s| s.strategy.0).collect::<Vec<_>>(),
        vec![2, 0, 1]
    );
    assert_eq!(
        m.subscribers(1, DataKind::Trade)[0].cadence,
        Cadence::Sampled { period_ns: 5_000_000 }
    );
    assert_eq!(m.unsubscribe(1, DataKind::Trade, StrategyIndex(0)), Ok(()));
    assert_eq!(m.unsubscribe(1, DataKind::Trade, StrategyIndex(0)), Err(Status::NotFound));
    assert_eq!(
        m.subscribers(1, DataKind::Trade).iter().map(|s| s.strategy.0).collect::<Vec<_>>(),
        vec![2, 1]
    );
    assert_eq!(
        m.subscribe(4, DataKind::Trade, StrategyIndex(0), Cadence::Every),
        Err(Status::OutOfRange)
    );
    assert!(m.subscribers(9, DataKind::Quote).is_empty());

    let bytes = save_state(&m).unwrap();
    let mut back = SubscriptionMatrix::new(4, 3);
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(save_state(&back).unwrap(), bytes);
    assert_eq!(
        back.find(1, DataKind::Trade, StrategyIndex(1)).map(|s| s.cadence),
        Some(Cadence::OnBatch)
    );
    let mut other = SubscriptionMatrix::new(5, 3);
    assert_eq!(load_state(&mut other, &bytes), Err(Status::CapacityExceeded));
}

#[test]
fn routing_interns_and_classifies() {
    let mut instruments = InstrumentTable::with_capacity(8);
    let mut bars: BarTable = BarTable::with_capacity(512);
    let mut kinds = std::collections::BTreeSet::new();
    let mut routed = 0;
    for record in Corpus::new(11).records(500) {
        let (_, event) = record.unwrap();
        match route_of(&event, &mut instruments, &mut bars).unwrap() {
            Some(route) => {
                routed += 1;
                kinds.insert(route.kind);
                if route.kind == DataKind::Bar {
                    assert!(bars.key_of(route.row).is_some());
                } else {
                    assert!(instruments.id_of(InstrumentSlot(route.row)).is_some());
                }
            }
            None => assert!(matches!(
                event,
                Event::Order(_)
                    | Event::AccountState(_)
                    | Event::RateLimitFeedback(_)
                    | Event::TimerFired(_)
                    | Event::BatchEnd(_)
                    | Event::NodeLifecycle(_)
                    | Event::Shutdown(_)
            )),
        }
    }
    assert!(routed > 200);
    assert_eq!(kinds.len(), DataKind::COUNT - 1, "every kind but Feature comes from input events");
    assert_eq!(instruments.len(), 5, "the corpus's five symbols, in first-seen order");
}

// ---- order book ---------------------------------------------------------------------------------

#[test]
fn book_applies_deltas_and_reports_levels_from_the_touch() {
    let mut b = book();
    assert!(b.is_empty());
    b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, "100.0", "1", 1)).unwrap();
    b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, "99.9", "2", 2)).unwrap();
    b.apply_delta(&delta(BookAction::Add, OrderSide::Sell, "100.2", "3", 3)).unwrap();
    b.apply_delta(&delta(BookAction::Add, OrderSide::Sell, "100.1", "4", 4)).unwrap();
    assert_eq!(b.best_bid().map(|l| l.price.to_string()), Some("100.0".into()));
    assert_eq!(
        b.best_ask().map(|l| (l.price.to_string(), l.size.to_string())),
        Some(("100.1".into(), "4.000".into()))
    );
    assert_eq!(b.spread_ticks(), Some(1));
    assert_eq!(
        levels(&b, Side::Bid),
        vec![("100.0".into(), "1.000".into()), ("99.9".into(), "2.000".into())]
    );
    assert_eq!(
        levels(&b, Side::Ask),
        vec![("100.1".into(), "4.000".into()), ("100.2".into(), "3.000".into())]
    );
    b.apply_delta(&delta(BookAction::Update, OrderSide::Buy, "100.0", "5", 5)).unwrap();
    assert_eq!(b.size_at(Side::Bid, price("100.0")).to_string(), "5.000");
    b.apply_delta(&delta(BookAction::Delete, OrderSide::Buy, "100.0", "0", 6)).unwrap();
    assert_eq!(b.best_bid().map(|l| l.price.to_string()), Some("99.9".into()));
    assert_eq!(b.depth_size(Side::Ask, 2).to_string(), "7.000");
    assert_eq!(b.sequence(), 6);
    assert_eq!(
        b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, "100.05", "1", 7)),
        Err(Status::InvalidArgument),
        "off the tick grid"
    );
    assert_eq!(
        b.apply_quote(&quote("1", "2", "1", "1", 8)),
        Err(Status::InvalidState),
        "an L2 book does not take quotes"
    );
    b.apply_delta(&OrderBookDelta::clear(id("BTCUSDT-PERP.BINANCE"), 9, ts(9), ts(9)).unwrap())
        .unwrap();
    assert!(b.is_empty());
}

#[test]
fn book_keeps_far_levels_in_the_overflow_and_recentres() {
    let mut b = OrderBook::new(BookConfig {
        price_increment: price("0.1"),
        size_precision: 3,
        window_levels: 64,
        overflow_levels: 128,
    })
    .unwrap();
    // 64 ticks of window; levels 300 ticks apart must both survive.
    b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, "100.0", "1", 1)).unwrap();
    b.apply_delta(&delta(BookAction::Add, OrderSide::Sell, "130.0", "2", 2)).unwrap();
    b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, "70.0", "3", 3)).unwrap();
    assert_eq!(
        levels(&b, Side::Bid),
        vec![("100.0".into(), "1.000".into()), ("70.0".into(), "3.000".into())]
    );
    assert_eq!(b.best_ask().map(|l| l.price.to_string()), Some("130.0".into()));
    // The market walks up 50 ticks at a time; everything stays queryable and exact.
    for i in 1..=40u64 {
        let bid = format!("{}.0", 100 + 5 * i);
        let ask = format!("{}.0", 100 + 5 * i + 1);
        b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, &bid, "1", 10 + 2 * i)).unwrap();
        b.apply_delta(&delta(BookAction::Add, OrderSide::Sell, &ask, "1", 11 + 2 * i)).unwrap();
        assert_eq!(b.best_bid().map(|l| l.price.to_string()), Some(bid.clone()));
    }
    assert_eq!(
        b.size_at(Side::Bid, price("70.0")).to_string(),
        "3.000",
        "a level 230 ticks below the touch is still there"
    );
    assert_eq!(b.size_at(Side::Ask, price("130.0")).to_string(), "2.000");
    let bids = levels(&b, Side::Bid);
    assert_eq!(bids.len(), 42);
    assert!(
        bids.windows(2).all(|w| price(&w[0].0) > price(&w[1].0)),
        "bids descend from the touch"
    );
}

#[test]
fn book_matches_a_sorted_reference_under_random_deltas() {
    for_all(|gen: &mut Gen| {
        let mut b = OrderBook::new(BookConfig {
            price_increment: price("1"),
            size_precision: 0,
            window_levels: 64,
            overflow_levels: 256,
        })
        .unwrap();
        let mut bids: Vec<(i64, u64)> = Vec::new(); // (tick, size), sorted ascending
        let mut asks: Vec<(i64, u64)> = Vec::new();
        for seq in 1..=200u64 {
            let side = if gen.coin() { OrderSide::Buy } else { OrderSide::Sell };
            let tick = gen.range(1, 400);
            let size = gen.range_u(0, 5);
            let action = if size == 0 { BookAction::Delete } else { BookAction::Update };
            let d = delta(action, side, &tick.to_string(), &size.to_string(), seq);
            b.apply_delta(&d).unwrap();
            let size = size * 1_000_000_000; // the book reports raw sizes
            let model = if side == OrderSide::Buy { &mut bids } else { &mut asks };
            match model.binary_search_by_key(&tick, |&(t, _)| t) {
                Ok(i) => {
                    if size == 0 {
                        model.remove(i);
                    } else {
                        model[i].1 = size;
                    }
                }
                Err(i) => {
                    if size != 0 {
                        model.insert(i, (tick, size));
                    }
                }
            }
            let got_bids: Vec<(i64, u64)> = b.ticks(Side::Bid).collect();
            let want_bids: Vec<(i64, u64)> = bids.iter().rev().copied().collect();
            assert_eq!(got_bids, want_bids, "bids after {seq} updates");
            let got_asks: Vec<(i64, u64)> = b.ticks(Side::Ask).collect();
            assert_eq!(got_asks, asks, "asks after {seq} updates");
        }
        // The snapshot round-trips to the same levels.
        let bytes = save_state(&b).unwrap();
        let mut back = OrderBook::new(*b.config()).unwrap();
        load_state(&mut back, &bytes).unwrap();
        assert_eq!(
            back.ticks(Side::Bid).collect::<Vec<_>>(),
            b.ticks(Side::Bid).collect::<Vec<_>>()
        );
        assert_eq!(
            back.ticks(Side::Ask).collect::<Vec<_>>(),
            b.ticks(Side::Ask).collect::<Vec<_>>()
        );
        assert_eq!(save_state(&back).unwrap(), bytes);
    });
}

#[test]
fn l1_book_takes_quotes() {
    let mut b = book();
    b.apply_quote(&quote("100.0", "100.1", "1", "2", 1)).unwrap();
    assert_eq!(b.best_bid().map(|l| l.size.to_string()), Some("1.000".into()));
    b.apply_quote(&quote("100.2", "100.3", "3", "4", 2)).unwrap();
    assert_eq!(levels(&b, Side::Bid), vec![("100.2".into(), "3.000".into())]);
    assert_eq!(levels(&b, Side::Ask), vec![("100.3".into(), "4.000".into())]);
    assert_eq!(
        b.apply_delta(&delta(BookAction::Add, OrderSide::Buy, "1.0", "1", 3)),
        Err(Status::InvalidState)
    );
}

// ---- bars ---------------------------------------------------------------------------------------

fn bar_type(spec: &str) -> BarType {
    BarType::parse(&format!("BTCUSDT-PERP.BINANCE-{spec}-INTERNAL")).unwrap()
}

#[test]
fn time_bars_align_to_the_epoch_and_close_at_their_end() {
    let mut agg = BarAggregator::new(bar_type("1-SECOND-LAST"), 1, 3).unwrap();
    let s = 1_000_000_000u64;
    assert!(agg.on_trade(&trade("100.0", "1", 10 * s + 300)).unwrap().is_empty());
    assert_eq!(agg.next_close(), Some(ts(11 * s)));
    assert!(agg.on_trade(&trade("101.0", "2", 10 * s + 900)).unwrap().is_empty());
    assert!(agg.on_trade(&trade("99.0", "1", 10 * s + 950)).unwrap().is_empty());
    // A trade in the next interval closes the bar first; the bar's timestamps are the interval end.
    let done = agg.on_trade(&trade("98.0", "1", 11 * s + 100)).unwrap();
    let bars: Vec<_> = done.iter().collect();
    assert_eq!(bars.len(), 1);
    let bar = bars[0];
    assert_eq!(
        (bar.open.to_string(), bar.high.to_string(), bar.low.to_string(), bar.close.to_string()),
        ("100.0".into(), "101.0".into(), "99.0".into(), "99.0".into())
    );
    assert_eq!(bar.volume.to_string(), "4.000");
    assert_eq!((bar.ts_event, bar.ts_init), (ts(11 * s), ts(11 * s)));
    // The timer closes the open bar without a trade.
    assert!(agg.on_time(ts(11 * s + 500)).is_none());
    let closed = agg.on_time(ts(12 * s)).unwrap();
    assert_eq!(closed.close.to_string(), "98.0");
    assert_eq!(agg.next_close(), None, "no open bar until the next trade");
    // Snapshot round trip of an open bar.
    agg.on_trade(&trade("97.0", "1", 12 * s + 1)).unwrap();
    let bytes = save_state(&agg).unwrap();
    let mut back = BarAggregator::new(bar_type("1-SECOND-LAST"), 1, 3).unwrap();
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(back.next_close(), Some(ts(13 * s)));
}

#[test]
fn tick_and_volume_bars_count_updates_and_split_large_trades() {
    let mut ticks = BarAggregator::new(bar_type("3-TICK-LAST"), 1, 3).unwrap();
    assert!(ticks.on_trade(&trade("1.0", "1", 1)).unwrap().is_empty());
    assert!(ticks.on_trade(&trade("3.0", "1", 2)).unwrap().is_empty());
    let done = ticks.on_trade(&trade("2.0", "1", 3)).unwrap();
    let bar = done.iter().next().unwrap();
    assert_eq!(
        (
            bar.open.to_string(),
            bar.high.to_string(),
            bar.low.to_string(),
            bar.close.to_string(),
            bar.volume.to_string()
        ),
        ("1.0".into(), "3.0".into(), "1.0".into(), "2.0".into(), "3.000".into())
    );
    assert_eq!(bar.ts_init, ts(3));

    let mut volume = BarAggregator::new(bar_type("2-VOLUME-LAST"), 1, 3).unwrap();
    assert!(volume.on_trade(&trade("1.0", "1", 1)).unwrap().is_empty());
    // A 5-unit trade completes the open bar (1 more unit), then two whole bars, and leaves 0.
    let done = volume.on_trade(&trade("2.0", "5", 2)).unwrap();
    let vols: Vec<String> = done.iter().map(|b| b.volume.to_string()).collect();
    assert_eq!(vols, vec!["2.000", "2.000", "2.000"]);
    assert_eq!(done.iter().next().unwrap().open.to_string(), "1.0");
    assert_eq!(volume.next_close(), None);
    let done = volume.on_trade(&trade("3.0", "0.5", 3)).unwrap();
    assert!(done.is_empty(), "the remainder of 0 started a fresh bar with 0.5");

    assert_eq!(
        BarAggregator::new(bar_type("2-VOLUME-MID"), 1, 3).err(),
        Some(Status::InvalidArgument)
    );
    assert!(BarAggregator::new(
        BarType::parse("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL").unwrap(),
        1,
        3
    )
    .is_err());
    let mut mid = BarAggregator::new(bar_type("2-TICK-MID"), 1, 3).unwrap();
    assert!(mid.on_quote(&quote("100.0", "100.3", "1", "1", 1)).unwrap().is_empty());
    let done = mid.on_quote(&quote("100.0", "100.1", "1", "1", 2)).unwrap();
    let bar = done.iter().next().unwrap();
    assert_eq!(
        (bar.open.to_string(), bar.close.to_string()),
        ("100.1".into(), "100.0".into()),
        "mids truncate onto the price grid"
    );
    assert_eq!(mid.on_trade(&trade("1.0", "1", 3)).err(), Some(Status::InvalidState));
}

// ---- features -----------------------------------------------------------------------------------

#[test]
fn features_compute_in_fixed_point() {
    let slot = InstrumentSlot(0);
    let mut graph = FeatureGraph::with_capacity(8);
    let ema = graph.declare(FeatureSpec { kind: FeatureKind::Ema { period: 3 }, slot }).unwrap();
    assert_eq!(
        graph.declare(FeatureSpec { kind: FeatureKind::Ema { period: 3 }, slot }),
        Ok(ema),
        "identical specs share a feature"
    );
    let vwap = graph.declare(FeatureSpec { kind: FeatureKind::Vwap { window: 2 }, slot }).unwrap();
    let vol =
        graph.declare(FeatureSpec { kind: FeatureKind::RealizedVol { window: 4 }, slot }).unwrap();
    let imb = graph.declare(FeatureSpec { kind: FeatureKind::Imbalance, slot }).unwrap();
    let micro = graph.declare(FeatureSpec { kind: FeatureKind::Microprice, slot }).unwrap();
    assert_eq!(graph.len(), 5);

    let mut out: Vec<(u32, String)> = Vec::new();
    let mut emit = |id: data::features::FeatureId, v: FeatureValue| out.push((id.0, v.to_string()));
    graph.on_trade(slot, &trade("100.0", "1", 1), &mut emit).unwrap();
    graph.on_trade(slot, &trade("104.0", "3", 2), &mut emit).unwrap();
    graph.on_trade(slot, &trade("102.0", "1", 3), &mut emit).unwrap();
    graph.on_quote(slot, &quote("100.0", "100.2", "3", "1", 4), &mut emit).unwrap();
    let value = |id: data::features::FeatureId| {
        out.iter().filter(|(i, _)| *i == id.0).map(|(_, v)| v.clone()).collect::<Vec<_>>()
    };
    // EMA: 100; 100 + (104-100)*2/4 = 102; 102 + (102-102)*2/4 = 102.
    assert_eq!(value(ema), vec!["100.000000000", "102.000000000", "102.000000000"]);
    // VWAP over 2 trades: 100; (100*1+104*3)/4 = 103; (104*3+102*1)/4 = 103.5.
    assert_eq!(value(vwap), vec!["100.000000000", "103.000000000", "103.500000000"]);
    // Realized vol: first trade no return; r1 = 0.04, vol = 0.04; r2 = -2/104, vol = sqrt(0.04^2 + r2^2).
    let v = value(vol);
    assert_eq!(v[0], "0.040000000");
    assert!(v[1].starts_with("0.0443"), "{}", v[1]);
    // Imbalance (3-1)/(3+1) = 0.5; microprice (100.0*1 + 100.2*3)/4 = 100.15.
    assert_eq!(value(imb), vec!["0.500000000"]);
    assert_eq!(value(micro), vec!["100.150000000"]);
    assert_eq!(graph.get(ema).unwrap().last(), Some(FeatureValue(102_000_000_000)));

    let bytes = save_state(&graph).unwrap();
    let mut back = FeatureGraph::with_capacity(8);
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(save_state(&back).unwrap(), bytes);
    assert_eq!(back.len(), 5);
    assert_eq!(
        FeatureGraph::with_capacity(1)
            .declare(FeatureSpec { kind: FeatureKind::Ema { period: 0 }, slot }),
        Err(Status::InvalidArgument)
    );
}

#[test]
fn imbalance_stays_within_bounds_and_microprice_within_the_spread() {
    for_all(|gen: &mut Gen| {
        let slot = InstrumentSlot(0);
        let mut graph = FeatureGraph::with_capacity(2);
        graph.declare(FeatureSpec { kind: FeatureKind::Imbalance, slot }).unwrap();
        graph.declare(FeatureSpec { kind: FeatureKind::Microprice, slot }).unwrap();
        let bid = gen.range_u(1, 1_000_000);
        let ask = bid + gen.range_u(1, 1000);
        let q = QuoteTick::new(
            id("BTCUSDT-PERP.BINANCE"),
            Price::from_units(bid as i64, 2).unwrap(),
            Price::from_units(ask as i64, 2).unwrap(),
            Quantity::from_units(gen.range_u(0, 1_000_000), 3).unwrap(),
            Quantity::from_units(gen.range_u(0, 1_000_000), 3).unwrap(),
            ts(1),
            ts(1),
        )
        .unwrap();
        let mut values = Vec::new();
        graph.on_quote(slot, &q, |id, v| values.push((id.0, v.0))).unwrap();
        for (id, v) in values {
            if id == 0 {
                assert!((-1_000_000_000..=1_000_000_000).contains(&v));
            } else {
                assert!(
                    q.bid_price.raw() <= v && v <= q.ask_price.raw(),
                    "microprice {v} outside [{}, {}]",
                    q.bid_price.raw(),
                    q.ask_price.raw()
                );
            }
        }
    });
}

// ---- golden -------------------------------------------------------------------------------------

/// Everything the layer derives from the seed 7 corpus, hashed: best bid and ask after each book
/// update, every bar the aggregators close, every feature value. The golden file changes only with
/// a deliberate change to the layer or to the corpus.
#[test]
#[allow(clippy::too_many_lines)] // one pass over the corpus feeding every component
fn seed7_derived_data_is_golden() {
    let expected = include_str!("../../../../tests/golden/data_seed7.fingerprint").trim();
    let mut hasher = Sha256::new();
    let mut count = 0u64;
    let mut instruments = InstrumentTable::with_capacity(8);
    let mut books: Vec<OrderBook> = Vec::new();
    let mut graph = FeatureGraph::with_capacity(64);
    let mut aggregators: Vec<Vec<BarAggregator>> = Vec::new();
    let feed = |bytes: &[u8], hasher: &mut Sha256, count: &mut u64| {
        hasher.update(bytes);
        *count += 1;
    };
    for record in Corpus::new(7).records(20_000) {
        let (_, event) = record.unwrap();
        let instrument_id = match &event {
            Event::TradeTick(e) => e.instrument_id,
            Event::QuoteTick(e) => e.instrument_id,
            Event::OrderBookDeltas(e) => e.instrument_id,
            _ => continue,
        };
        let slot = instruments.intern(instrument_id).unwrap();
        while aggregators.len() <= slot.index() {
            let iid = instruments.id_of(InstrumentSlot(aggregators.len() as u32)).unwrap();
            let precision = corpus::price_precision(iid.symbol.as_str());
            books.push(
                OrderBook::new(BookConfig {
                    price_increment: Price::from_units(1, precision).unwrap(),
                    size_precision: 3,
                    window_levels: 128,
                    overflow_levels: 512,
                })
                .unwrap(),
            );
            let iid = iid.to_string();
            let make = |spec: &str| {
                BarAggregator::new(
                    BarType::parse(&format!("{iid}-{spec}-INTERNAL")).unwrap(),
                    precision,
                    3,
                )
                .unwrap()
            };
            aggregators.push(vec![
                make("1-MINUTE-LAST"),
                make("5-TICK-LAST"),
                make("100-VOLUME-LAST"),
                make("3-TICK-MID"),
            ]);
            for kind in [
                FeatureKind::Ema { period: 10 },
                FeatureKind::Vwap { window: 5 },
                FeatureKind::RealizedVol { window: 8 },
                FeatureKind::Imbalance,
                FeatureKind::Microprice,
            ] {
                graph
                    .declare(FeatureSpec {
                        kind,
                        slot: InstrumentSlot(aggregators.len() as u32 - 1),
                    })
                    .unwrap();
            }
        }
        match &event {
            Event::TradeTick(t) => {
                for agg in aggregators[slot.index()].iter_mut().filter(|a| a.uses_trades()) {
                    for bar in agg.on_trade(t).unwrap().iter() {
                        feed(&bar.to_wire(), &mut hasher, &mut count);
                    }
                }
                graph
                    .on_trade(slot, t, |id, v| {
                        feed(
                            &[id.0.to_le_bytes().as_slice(), &v.0.to_le_bytes()].concat(),
                            &mut hasher,
                            &mut count,
                        );
                    })
                    .unwrap();
            }
            Event::QuoteTick(q) => {
                for agg in aggregators[slot.index()].iter_mut().filter(|a| !a.uses_trades()) {
                    for bar in agg.on_quote(q).unwrap().iter() {
                        feed(&bar.to_wire(), &mut hasher, &mut count);
                    }
                }
                graph
                    .on_quote(slot, q, |id, v| {
                        feed(
                            &[id.0.to_le_bytes().as_slice(), &v.0.to_le_bytes()].concat(),
                            &mut hasher,
                            &mut count,
                        );
                    })
                    .unwrap();
            }
            Event::OrderBookDeltas(d) => {
                let b = &mut books[slot.index()];
                b.apply_deltas(d).unwrap();
                let mut top = [BookLevel::default(); 2];
                let n = b.levels(Side::Bid, &mut top[..1]) + b.levels(Side::Ask, &mut top[1..]);
                let mut bytes = Vec::new();
                for level in &top[..n] {
                    bytes.extend_from_slice(&level.price.to_wire());
                    bytes.extend_from_slice(&level.size.to_wire());
                }
                feed(&bytes, &mut hasher, &mut count);
            }
            _ => {}
        }
    }
    let digest = hasher.finish();
    let hex = String::from_utf8(kernel_core::sha256::hex(&digest).to_vec()).unwrap();
    let actual = format!("{count} {hex}");
    assert_eq!(actual, expected, "derived-data fingerprint of the seed 7 corpus");
}
