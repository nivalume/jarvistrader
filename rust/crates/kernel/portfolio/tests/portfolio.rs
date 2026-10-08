//! Positions, balances, margin and funding: exact integer accounting, the ledger invariant (venue
//! position equals the sum of the strategies' shares), flips split into two parts, and snapshot
//! round trips.

use cost::slippage::unit_multiplier;
use cost::Rate;
use data::intern::InstrumentSlot;
use data::subscription::StrategyIndex;
use kernel_core::{load_state, save_state, FixedVec, Status, UnixNanos};
use model::account::{AccountBalance, AccountState};
use model::data::FundingRateUpdate;
use model::enums::{AccountType, AssetClass, LiquiditySide, OrderSide, OrderType, PositionSide};
use model::instruments::{Instrument, InstrumentKind, InstrumentLimits};
use model::order_events::{FillInfoFlags, OrderEventHeader, OrderFilled};
use model::{
    AccountId, ClientOrderId, Currency, InstrumentId, Money, Price, Quantity, StrategyId, Symbol,
    TradeId, TraderId, Uuid4, VenueOrderId,
};
use portfolio::{MarginModel, NettingPosition, Portfolio, PortfolioConfig, PositionStep};
use testkit::{for_all, Gen};

fn usdt() -> Currency {
    Currency::builtin_by_code("USDT").unwrap()
}
fn perp() -> Instrument {
    Instrument::new(
        InstrumentId::parse("BTCUSDT-PERP.BINANCE").unwrap(),
        Symbol::new("BTCUSDT").unwrap(),
        AssetClass::Cryptocurrency,
        InstrumentKind::CryptoPerpetual { settlement_currency: usdt(), is_inverse: false },
        Currency::builtin_by_code("BTC"),
        usdt(),
        Price::parse("0.1").unwrap(),
        Quantity::parse("0.001").unwrap(),
        unit_multiplier(),
        None,
        InstrumentLimits::default(),
        Quantity::parse("0.1").unwrap(),
        Quantity::parse("0.05").unwrap(),
        UnixNanos::new(1),
        UnixNanos::new(1),
    )
    .unwrap()
}
fn spot() -> Instrument {
    let mut i = perp();
    i.kind = InstrumentKind::CurrencyPair;
    i
}
fn p(s: &str) -> Price {
    Price::parse(s).unwrap()
}
fn q(s: &str) -> Quantity {
    Quantity::parse(s).unwrap()
}
fn ts(n: u64) -> UnixNanos {
    UnixNanos::new(n)
}
fn oid(n: u64) -> ClientOrderId {
    model::client_order_id::format("t", 1, n).unwrap()
}
fn fill(side: OrderSide, size: &str, price: &str, commission: Option<&str>, n: u64) -> OrderFilled {
    OrderFilled {
        header: OrderEventHeader {
            trader_id: TraderId::new("T-1").unwrap(),
            strategy_id: StrategyId::new("S-1").unwrap(),
            instrument_id: InstrumentId::parse("BTCUSDT-PERP.BINANCE").unwrap(),
            client_order_id: oid(n),
            event_id: Uuid4::from_u64s(n, 1),
            ts_event: ts(n),
            ts_init: ts(n),
            causation_id: None,
        },
        venue_order_id: VenueOrderId::new("1").unwrap(),
        account_id: AccountId::new("BINANCE-1").unwrap(),
        trade_id: TradeId::new(&n.to_string()).unwrap(),
        order_side: side,
        order_type: OrderType::Limit,
        last_qty: q(size),
        last_px: p(price),
        currency: usdt(),
        liquidity_side: LiquiditySide::Maker,
        reconciliation: false,
        position_id: None,
        commission: commission.map(|c| Money::parse_amount(c, usdt()).unwrap()),
        info_flags: FillInfoFlags::default(),
    }
}
fn portfolio(strategies: u16) -> Portfolio {
    Portfolio::new(PortfolioConfig {
        instruments: 2,
        strategies,
        margin: MarginModel::Standard,
        currencies: 4,
    })
}
const SLOT: InstrumentSlot = InstrumentSlot(0);
const S0: StrategyIndex = StrategyIndex(0);
const S1: StrategyIndex = StrategyIndex(1);
const ONE: u64 = 1_000_000_000;

// ---- position -----------------------------------------------------------------------------------

#[test]
fn netting_position_keeps_an_exact_average_and_realizes_exactly() {
    let mut pos = NettingPosition::default();
    assert_eq!(pos.closable(OrderSide::Sell), 0);
    let a = pos.apply(OrderSide::Buy, q("1"), p("100"), ONE, oid(1), ts(1)).unwrap();
    assert!(a.opened);
    pos.apply(OrderSide::Buy, q("1"), p("110"), ONE, oid(2), ts(2)).unwrap();
    assert_eq!(pos.side(), PositionSide::Long);
    assert_eq!(pos.avg_px_open().unwrap().to_string(), "105.000000000");
    assert_eq!(pos.closable(OrderSide::Sell), 2 * ONE);
    assert_eq!(pos.closable(OrderSide::Buy), 0);
    assert_eq!(pos.unrealized(p("120"), ONE).unwrap(), 30 * ONE as i64);
    // Partial close at 120: realizes (120 - 105) x 1 = 15; the average stays 105.
    let r = pos.apply(OrderSide::Sell, q("1"), p("120"), ONE, oid(3), ts(3)).unwrap();
    assert_eq!(r.realized_raw, 15 * ONE as i64);
    assert!(!r.closed);
    assert_eq!(pos.avg_px_open().unwrap().to_string(), "105.000000000");
    assert_eq!(pos.avg_px_close().unwrap().to_string(), "120.000000000");
    // Full close at 90: realizes (90 - 105) = -15; total realized 0; the entry average survives.
    let r = pos.apply(OrderSide::Sell, q("1"), p("90"), ONE, oid(4), ts(4)).unwrap();
    assert_eq!(r.realized_raw, -15 * ONE as i64);
    assert!(r.closed);
    assert!(!pos.is_open());
    assert_eq!(pos.realized_raw(), 0);
    assert_eq!(pos.open_notional(), 0, "a full close leaves no entry cost behind");
    assert_eq!(pos.avg_px_open().unwrap().to_string(), "105.000000000");
    assert_eq!(pos.unrealized(p("1"), ONE).unwrap(), 0);
    // Over-closing is the caller's error.
    pos.apply(OrderSide::Sell, q("1"), p("100"), ONE, oid(5), ts(5)).unwrap();
    assert_eq!(
        pos.apply(OrderSide::Buy, q("2"), p("100"), ONE, oid(6), ts(6)).err(),
        Some(Status::InvalidArgument)
    );
    // A short at 100 bought back at 90 gains 10.
    let r = pos.apply(OrderSide::Buy, q("1"), p("90"), ONE, oid(7), ts(7)).unwrap();
    assert_eq!(r.realized_raw, 10 * ONE as i64);
    assert_eq!(pos.total_realized_raw(), 10 * ONE as i64);
}

#[test]
fn reducing_removes_entry_cost_in_proportion_with_odd_quantities() {
    let mut pos = NettingPosition::default();
    pos.apply(OrderSide::Buy, q("3"), p("100.1"), ONE, oid(1), ts(1)).unwrap(); // notional 300.3
    let r = pos.apply(OrderSide::Sell, q("1"), p("100.1"), ONE, oid(2), ts(2)).unwrap();
    assert_eq!(r.realized_raw, 0, "selling at the average realizes nothing");
    assert_eq!(pos.avg_px_open().unwrap().to_string(), "100.100000000");
    pos.apply(OrderSide::Sell, q("2"), p("100.1"), ONE, oid(3), ts(3)).unwrap();
    assert_eq!(pos.open_notional(), 0);
    // A contract multiplier of 0.01 scales PnL.
    let mut mini = NettingPosition::default();
    mini.apply(OrderSide::Buy, q("100"), p("50000"), ONE / 100, oid(1), ts(1)).unwrap();
    let r = mini.apply(OrderSide::Sell, q("100"), p("50100"), ONE / 100, oid(2), ts(2)).unwrap();
    assert_eq!(r.realized_raw, 100 * ONE as i64, "100 contracts x 100 x 0.01");
}

#[test]
fn position_state_round_trips() {
    let mut pos = NettingPosition::default();
    pos.apply(OrderSide::Sell, q("1.5"), p("100"), ONE, oid(1), ts(1)).unwrap();
    pos.add_commission(5).unwrap();
    pos.add_funding(-7).unwrap();
    let bytes = save_state(&pos).unwrap();
    let mut back = NettingPosition::default();
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(back, pos);
}

// ---- portfolio ----------------------------------------------------------------------------------

#[test]
fn fills_move_positions_balances_and_split_flips() {
    let mut pf = portfolio(2);
    let perp = perp();
    let mut balances = FixedVec::with_capacity(1);
    let m = |s: &str| Money::parse_amount(s, usdt()).unwrap();
    balances.push(AccountBalance::new(m("1000"), m("0"), m("1000")).unwrap()).unwrap();
    let account = AccountState::new(
        AccountId::new("BINANCE-1").unwrap(),
        AccountType::Margin,
        Some(usdt()),
        balances,
        FixedVec::with_capacity(0),
        true,
        Uuid4::from_u64s(1, 1),
        ts(1),
        ts(1),
    )
    .unwrap();
    pf.set_account(&account).unwrap();
    assert_eq!(pf.wallet(usdt()).unwrap().to_string(), "1000.00000000 USDT");

    let out = pf
        .on_fill(&perp, SLOT, S0, &fill(OrderSide::Buy, "2", "100", Some("0.04"), 1))
        .unwrap()
        .unwrap();
    assert_eq!(out.iter().map(|p| p.step).collect::<Vec<_>>(), vec![PositionStep::Opened]);
    assert_eq!(
        pf.wallet(usdt()).unwrap().to_string(),
        "999.96000000 USDT",
        "the commission leaves the wallet"
    );
    assert_eq!(pf.position(S0, SLOT).unwrap().signed_raw(), 2 * ONE as i64);
    // Strategy 1 sells 1: the venue nets to +1; strategy 1 is short 1.
    pf.on_fill(&perp, SLOT, S1, &fill(OrderSide::Sell, "1", "110", None, 2)).unwrap();
    assert_eq!(pf.venue(SLOT).unwrap().signed_raw(), ONE as i64);
    assert_eq!(pf.position(S1, SLOT).unwrap().side(), PositionSide::Short);
    assert!(pf.ledger_is_consistent());
    // Strategy 0 sells 3 at 120: closes its 2 (realized 40) and opens a short of 1; two parts.
    let out =
        pf.on_fill(&perp, SLOT, S0, &fill(OrderSide::Sell, "3", "120", None, 3)).unwrap().unwrap();
    let parts: Vec<_> =
        out.iter().map(|p| (p.step, p.quantity.to_string(), p.realized.to_string())).collect();
    assert_eq!(
        parts,
        vec![
            (PositionStep::Closed, "2".into(), "40.00000000 USDT".into()),
            (PositionStep::Opened, "1".into(), "0.00000000 USDT".into())
        ]
    );
    assert_eq!(pf.venue(SLOT).unwrap().signed_raw(), -2 * ONE as i64);
    assert!(pf.ledger_is_consistent());
    // The venue position realized (110 - 100) when strategy 1's sell reduced it, then (120 - 100)
    // when its remaining long of 1 (avg 100) closed: 30 in total.
    assert_eq!(pf.wallet(usdt()).unwrap().to_string(), "1029.96000000 USDT");
    assert_eq!(pf.stats().fills, 3);
    // Valuation and unrealized: no price yet, then a mark.
    assert!(pf.unrealized(&perp, SLOT, pf.position(S0, SLOT).unwrap()).unwrap().is_zero());
    pf.on_mark(SLOT, p("130")).unwrap();
    assert_eq!(
        pf.unrealized(&perp, SLOT, pf.position(S0, SLOT).unwrap()).unwrap().to_string(),
        "-10.00000000 USDT"
    );
    let (initial, maintenance) = pf.margins(&perp, SLOT).unwrap();
    assert_eq!(
        (initial.to_string(), maintenance.to_string()),
        ("26.00000000 USDT".into(), "13.00000000 USDT".into()),
        "2 x 130 notional at 10% / 5%"
    );
    // Spot fills are counted, not booked.
    assert!(pf
        .on_fill(&spot(), InstrumentSlot(1), S0, &fill(OrderSide::Buy, "1", "1", None, 4))
        .unwrap()
        .is_none());
    assert_eq!(pf.stats().fills_not_booked, 1);
    // A late commission.
    pf.on_commission(SLOT, S0, m("0.5")).unwrap();
    assert_eq!(pf.wallet(usdt()).unwrap().to_string(), "1029.46000000 USDT");
    assert_eq!(pf.position(S0, SLOT).unwrap().total_commission_raw(), 540_000_000);

    let bytes = save_state(&pf).unwrap();
    let mut back = portfolio(2);
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(save_state(&back).unwrap(), bytes);
    assert_eq!(back.wallet(usdt()), pf.wallet(usdt()));
}

#[test]
fn funding_settles_the_stored_rate_when_the_next_time_moves_on() {
    let mut pf = portfolio(2);
    let perp = perp();
    pf.on_fill(&perp, SLOT, S0, &fill(OrderSide::Buy, "2", "50000", None, 1)).unwrap();
    pf.on_fill(&perp, SLOT, S1, &fill(OrderSide::Sell, "1", "50000", None, 2)).unwrap();
    pf.on_mark(SLOT, p("50000")).unwrap();
    let update = |rate: &str, next: Option<u64>, t: u64| {
        FundingRateUpdate::new(perp.id, p(rate), Some(480), next.map(ts), ts(t), ts(t)).unwrap()
    };
    // A predicted rate: stored, nothing settles.
    assert!(pf.on_funding(&perp, SLOT, &update("0.0001", Some(1_000), 10)).unwrap().is_none());
    assert!(
        pf.on_funding(&perp, SLOT, &update("0.0002", Some(1_000), 20)).unwrap().is_none(),
        "same funding time: the prediction is refreshed"
    );
    // The funding time moved on: the stored 0.02% settles at the mark.
    let settlement =
        pf.on_funding(&perp, SLOT, &update("0.0003", Some(2_000), 30)).unwrap().unwrap();
    assert_eq!(settlement.rate, Rate::parse("0.0002").unwrap());
    // Venue is long 1 (2 - 1): pays 1 x 50000 x 0.0002 = 10.
    assert_eq!(settlement.venue.to_string(), "-10.00000000 USDT");
    let shares: Vec<_> = settlement.shares.iter().map(|(s, m)| (s.0, m.to_string())).collect();
    assert_eq!(shares, vec![(0, "-20.00000000 USDT".into()), (1, "10.00000000 USDT".into())]);
    assert_eq!(
        pf.wallet(usdt()).unwrap().to_string(),
        "-10.00000000 USDT",
        "no account snapshot: the balance starts from the flows"
    );
    assert_eq!(pf.stats().funding_settlements, 1);
    // A historical rate without next_funding settles itself.
    let historical =
        FundingRateUpdate::new(perp.id, p("-0.0001"), None, None, ts(40), ts(40)).unwrap();
    let s = pf.on_funding(&perp, SLOT, &historical).unwrap().unwrap();
    assert_eq!(s.venue.to_string(), "5.00000000 USDT", "a long receives a negative rate");
}

#[test]
fn margin_models_round_up_onto_the_currency_grid() {
    let perp = perp();
    let notional = 12_345_678_901u64; // 12.345678901 USDT on the 1e9 scale
    assert_eq!(
        MarginModel::Standard.initial(&perp, notional).unwrap().to_string(),
        "1.23456790 USDT",
        "10%, rounded up at 8 decimals"
    );
    assert_eq!(
        MarginModel::Standard.maintenance(&perp, notional).unwrap().to_string(),
        "0.61728395 USDT"
    );
    let lev = MarginModel::leveraged(3).unwrap();
    assert_eq!(lev.initial_fraction(&perp), 333_333_334, "1/3 rounded up");
    assert_eq!(
        lev.initial(&perp, 300 * ONE).unwrap().to_string(),
        "100.00000020 USDT",
        "300 x 0.333333334, up onto 8 decimals"
    );
    assert_eq!(MarginModel::leveraged(0).err(), Some(Status::InvalidArgument));
    let pf = portfolio(1);
    assert_eq!(pf.order_margin(&perp, p("100"), q("2.5")).unwrap().to_string(), "25.00000000 USDT");
}

#[test]
fn the_venue_position_is_always_the_sum_of_the_strategies() {
    for_all(|gen: &mut Gen| {
        let strategies = 3u16;
        let mut pf = Portfolio::new(PortfolioConfig {
            instruments: 1,
            strategies,
            margin: MarginModel::Standard,
            currencies: 2,
        });
        let perp = perp();
        // Reference: per strategy, netting in whole units with the entry notional on the 10^18
        // scale the kernel uses, so proportional removal floors at the same resolution; each
        // part's PnL truncates toward zero onto the 10^9 scale, as the kernel's does.
        const SCALE_18: i128 = 1_000_000_000_000_000_000;
        let mut reference_realized: i128 = 0;
        let mut model: Vec<(i64, i128)> = vec![(0, 0); strategies as usize]; // (signed units, open notional at 10^18)
        for n in 1..=40u64 {
            let s = StrategyIndex(gen.below(u64::from(strategies)) as u16);
            let side = if gen.coin() { OrderSide::Buy } else { OrderSide::Sell };
            let size = gen.range_u(1, 5);
            let price = gen.range_u(90, 110);
            let f = fill(side, &size.to_string(), &price.to_string(), None, n);
            pf.on_fill(&perp, SLOT, s, &f).unwrap();
            assert!(pf.ledger_is_consistent(), "after fill {n}");
            // Reference: per strategy, netting in whole units with an exact average.
            let (qty, notional) = &mut model[s.0 as usize];
            let mut remaining = size as i64;
            let sign: i64 = if side == OrderSide::Buy { 1 } else { -1 };
            if *qty != 0 && (*qty > 0) != (sign > 0) {
                let close = remaining.min(qty.abs());
                let entry = *notional * i128::from(close) / i128::from(qty.abs());
                let exit = i128::from(price) * i128::from(close) * SCALE_18;
                let diff = if *qty > 0 { exit - entry } else { entry - exit };
                reference_realized += diff / 1_000_000_000;
                *notional -= entry;
                *qty += sign * close; // toward flat
                remaining -= close;
            }
            if remaining > 0 {
                *qty += sign * remaining;
                *notional += i128::from(price) * i128::from(remaining) * SCALE_18;
            }
        }
        let realized: i128 = (0..strategies)
            .map(|s| i128::from(pf.position(StrategyIndex(s), SLOT).unwrap().total_realized_raw()))
            .sum();
        assert_eq!(realized, reference_realized, "strategies' realized PnL");
        let venue_qty: i64 = model.iter().map(|(q, _)| *q).sum();
        assert_eq!(pf.venue(SLOT).unwrap().signed_raw(), venue_qty * ONE as i64);
    });
}
