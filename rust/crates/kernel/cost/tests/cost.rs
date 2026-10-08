//! Fees, funding, slippage and latency: exact numbers, rounding against the account, and the
//! determinism of latency draws.

use cost::fees::{funding, MakerTakerFees, Rate};
use cost::latency::{JitteredLatency, LatencyHop};
use cost::slippage::{unit_multiplier, BookDepthSlippage};
use kernel_core::{DurationNanos, Status, UnixNanos};
use model::enums::{AssetClass, LiquiditySide, OrderSide};
use model::instruments::{Instrument, InstrumentKind, InstrumentLimits};
use model::{Currency, InstrumentId, Price, Quantity, Symbol};
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
fn p(s: &str) -> Price {
    Price::parse(s).unwrap()
}
fn q(s: &str) -> Quantity {
    Quantity::parse(s).unwrap()
}

#[test]
fn binance_schedules_charge_the_published_rates() {
    let fees = MakerTakerFees::schedule("binance_usdm_vip0").unwrap();
    // 10 000 USDT notional: taker 0.05% = 5, maker 0.02% = 2; paid is positive.
    assert_eq!(
        fees.commission(&perp(), p("10000"), q("1"), LiquiditySide::Taker).unwrap().to_string(),
        "5.00000000 USDT"
    );
    assert_eq!(
        fees.commission(&perp(), p("10000"), q("1"), LiquiditySide::Maker).unwrap().to_string(),
        "2.00000000 USDT"
    );
    assert_eq!(
        fees.commission(&perp(), p("10000"), q("1"), LiquiditySide::NoLiquiditySide)
            .unwrap()
            .to_string(),
        "5.00000000 USDT",
        "unknown side pays taker"
    );
    let bnb = MakerTakerFees::schedule("binance_usdm_vip0_bnb").unwrap();
    assert_eq!(
        bnb.commission(&perp(), p("10000"), q("1"), LiquiditySide::Taker).unwrap().to_string(),
        "4.50000000 USDT"
    );
    assert_eq!(MakerTakerFees::schedule("binance_spot_vip0").unwrap().taker(), Rate::bps(10));
    assert!(MakerTakerFees::schedule("zero")
        .unwrap()
        .commission(&perp(), p("10000"), q("1"), LiquiditySide::Taker)
        .unwrap()
        .is_zero());
    assert_eq!(MakerTakerFees::schedule("vip9").err(), Some(Status::NotFound));
    assert_eq!(Rate::parse("0.0002"), Ok(Rate::bps(2)));
    assert!(
        MakerTakerFees::new(Rate(0), Rate(-1), Rate(0)).is_err(),
        "a negative taker rate makes no sense"
    );
}

#[test]
fn fees_round_against_the_account() {
    let fees = MakerTakerFees::schedule("binance_usdm_vip0").unwrap();
    // 0.001 BTC at 0.3: notional 0.0003, taker fee 1.5e-7 = 0.00000015, exact on 8 decimals.
    assert_eq!(
        fees.commission(&perp(), p("0.3"), q("0.001"), LiquiditySide::Taker).unwrap().to_string(),
        "0.00000015 USDT"
    );
    // Notional 0.0001: fee 5e-8 = 0.00000005 exact; notional 0.00001: fee 5e-9 rounds UP to 1e-8.
    assert_eq!(
        fees.commission(&perp(), p("0.1"), q("0.0001"), LiquiditySide::Taker).unwrap().to_string(),
        "0.00000001 USDT"
    );
    // A maker rebate rounds DOWN: -0.01% of 0.00001 = -1e-9 -> 0.
    let rebate = MakerTakerFees::new(Rate::bps(-1), Rate::bps(5), Rate(0)).unwrap();
    assert_eq!(
        rebate
            .commission(&perp(), p("0.1"), q("0.0001"), LiquiditySide::Maker)
            .unwrap()
            .to_string(),
        "0.00000000 USDT"
    );
    assert_eq!(
        rebate.commission(&perp(), p("10000"), q("1"), LiquiditySide::Maker).unwrap().to_string(),
        "-1.00000000 USDT"
    );
}

#[test]
fn funding_flows_from_longs_to_shorts_at_a_positive_rate() {
    let perp = perp();
    let rate = Rate::parse("0.0001").unwrap(); // 0.01%
    let long = 2_000_000_000; // 2 BTC
                              // 2 BTC at 50 000 = 100 000 notional; 0.01% = 10 USDT; the long pays, the short receives.
    assert_eq!(funding(&perp, long, p("50000"), rate).unwrap().to_string(), "-10.00000000 USDT");
    assert_eq!(funding(&perp, -long, p("50000"), rate).unwrap().to_string(), "10.00000000 USDT");
    assert_eq!(
        funding(&perp, long, p("50000"), Rate(-rate.0)).unwrap().to_string(),
        "10.00000000 USDT"
    );
    assert!(funding(&perp, 0, p("50000"), rate).unwrap().is_zero());
    // Rounding: paid rounds up in magnitude, received rounds down.
    let tiny = Rate(1); // 1e-9
    assert_eq!(funding(&perp, 1_000_000, p("1"), tiny).unwrap().to_string(), "-0.00000001 USDT");
    assert_eq!(funding(&perp, -1_000_000, p("1"), tiny).unwrap().to_string(), "0.00000000 USDT");
}

#[test]
fn slippage_walks_the_book_and_rounds_against_the_taker() {
    let asks = [(p("100.0"), q("1")), (p("100.5"), q("2")), (p("101.0"), q("10"))];
    let e = BookDepthSlippage::estimate(OrderSide::Buy, q("2"), &asks).unwrap();
    assert_eq!(e.filled.to_string(), "2", "at the asked precision");
    assert_eq!(e.average_price.to_string(), "100.250000000");
    assert_eq!(e.worst_price.to_string(), "100.5");
    assert_eq!(e.levels, 2);
    assert_eq!(e.slippage_per_unit_raw, 250_000_000);
    assert_eq!(
        BookDepthSlippage::cost_raw(&e, unit_multiplier()).unwrap(),
        500_000_000,
        "0.25 x 2 units"
    );
    // Three units: 1 @100 + 2 @100.5 -> average 100.333..., a buyer rounds up to ...334.
    let e3 = BookDepthSlippage::estimate(OrderSide::Buy, q("3"), &asks).unwrap();
    assert_eq!(e3.average_price.to_string(), "100.333333334");
    // A seller walking bids rounds down.
    let bids = [(p("100.0"), q("1")), (p("99.5"), q("2"))];
    let s3 = BookDepthSlippage::estimate(OrderSide::Sell, q("3"), &bids).unwrap();
    assert_eq!(s3.average_price.to_string(), "99.666666666");
    // Not enough depth: filled reports what the book had.
    let short = BookDepthSlippage::estimate(OrderSide::Sell, q("10"), &bids).unwrap();
    assert_eq!(short.filled.to_string(), "3");
    assert_eq!(
        BookDepthSlippage::estimate(OrderSide::Buy, q("1"), &[] as &[(Price, Quantity)]).err(),
        Some(Status::InvalidArgument)
    );
    assert_eq!(
        BookDepthSlippage::estimate(OrderSide::Buy, q("0"), &asks).err(),
        Some(Status::InvalidArgument)
    );
}

#[test]
fn latency_is_a_pure_function_of_identity_hop_and_attempt() {
    let base = [
        DurationNanos::from_millis(5),
        DurationNanos::from_millis(20),
        DurationNanos::from_millis(15),
    ];
    let jitter = [
        DurationNanos::from_millis(1),
        DurationNanos::from_millis(10),
        DurationNanos::from_millis(10),
    ];
    let a = JitteredLatency::new(7, base, jitter);
    let b = JitteredLatency::new(7, base, jitter);
    assert_eq!(a.delay(42, LatencyHop::Outbound, 0), b.delay(42, LatencyHop::Outbound, 0));
    assert_ne!(a.delay(42, LatencyHop::Outbound, 0), a.delay(42, LatencyHop::Outbound, 1));
    assert_ne!(
        a.delay(42, LatencyHop::Outbound, 0),
        JitteredLatency::new(8, base, jitter).delay(42, LatencyHop::Outbound, 0)
    );
    for_all(|gen: &mut Gen| {
        let identity = gen.next_u64();
        for hop in LatencyHop::ALL {
            let d = a.delay(identity, hop, 0).value();
            let i = hop as usize - 1;
            assert!(
                d >= base[i].value() && d <= base[i].value() + jitter[i].value(),
                "{d} outside [{}, {}]",
                base[i].value(),
                base[i].value() + jitter[i].value()
            );
        }
    });
    let constant = JitteredLatency::constant(1, DurationNanos::from_millis(3));
    assert_eq!(constant.delay(1, LatencyHop::Feed, 0), DurationNanos::from_millis(3));
    assert_eq!(constant.delay(2, LatencyHop::Inbound, 9), DurationNanos::from_millis(3));
    assert!(a.validate().is_ok());
    assert!(JitteredLatency::new(1, [DurationNanos::new(u64::MAX); 3], [DurationNanos::new(1); 3])
        .validate()
        .is_err());
}
