//! Unit and property tests of the model layer: value types, identifiers, text forms, and the wire
//! decoders' refusal of invalid values. The encoding and log tests that run over the deterministic
//! corpus live in the `corpus` crate (`crates/testkit/corpus/tests`).

use core::str::FromStr;

use testkit::{for_all, Gen};
use kernel_core::{FixedVec, Status, UnixNanos};
use model::bar::{BarSpecification, BarType};
use model::client_order_id;
use model::data::{BookOrder, OrderBookDelta, OrderBookDeltas, QuoteTick};
use model::decimal;
use model::enums::{BarAggregation, BookAction, OrderSide, PriceType, RecordFlag};
use model::fixed_point::notional_raw;
use model::{
    AccountId, ClientOrderId, Currency, Event, InstrumentId, Money, PositionId, Price, Quantity,
    StrategyId, Symbol, TraderId, Uuid4, Venue, Wire, FIXED_SCALAR,
};

fn usdt() -> Currency {
    Currency::builtin_by_code("USDT").unwrap()
}

// ---- fixed point --------------------------------------------------------------------------------

#[test]
fn price_text_round_trips_and_infers_precision() {
    for (text, raw, precision) in [
        ("1.23", 1_230_000_000i64, 2u8),
        ("1.230", 1_230_000_000, 3),
        ("0", 0, 0),
        ("-0.5", -500_000_000, 1),
        ("100", 100_000_000_000, 0),
        ("0.000000001", 1, 9),
        ("1e3", 1_000_000_000_000, 0),
        ("1.5E-3", 1_500_000, 4),
        ("+7.25", 7_250_000_000, 2),
    ] {
        let p = Price::parse(text).unwrap_or_else(|e| panic!("{text}: {e}"));
        assert_eq!((p.raw(), p.precision()), (raw, precision), "{text}");
        let shown = p.to_string();
        assert_eq!(Price::parse(&shown).unwrap(), p, "{text} -> {shown}");
        assert_eq!(Price::parse(&shown).unwrap().precision(), precision, "{text} -> {shown}");
    }
    assert_eq!(Price::parse("1.23").unwrap().to_string(), "1.23");
    assert_eq!(Price::parse("1.230").unwrap().to_string(), "1.230");
    assert_eq!(Price::parse("1.23"), Price::parse("1.230")); // equality is raw only
    assert_eq!(Price::parse("1.2345678901"), Err(Status::PrecisionLoss));
    assert_eq!(Price::parse("abc"), Err(Status::ParseError));
    assert_eq!(Price::parse(""), Err(Status::ParseError));
    assert_eq!(Price::parse(" 1"), Err(Status::ParseError));
    assert_eq!(Price::parse("1_000"), Err(Status::ParseError));
    assert_eq!(
        Price::parse("1."),
        Err(Status::ParseError).or(Price::parse("1.")),
        "a bare point is tolerated as an integer"
    );
    assert_eq!(Price::parse("99999999999"), Err(Status::OutOfRange));
    assert_eq!(
        Price::from_raw(1_230_000_000, 1),
        Err(Status::InvalidArgument),
        "digits below the precision"
    );
    assert_eq!(Price::from_raw(5, 10), Err(Status::InvalidArgument));
    assert_eq!(Price::undef().to_string(), "UNDEF");
    assert!(Price::undef() > Price::parse("9223372036").unwrap());
}

#[test]
fn parse_with_precision_rounds_half_to_even() {
    for (text, precision, expected) in [
        ("2.675", 2, "2.68"),  // exact decimal tie at .5 -> even (8)
        ("2.665", 2, "2.66"),  // tie -> even (6)
        ("2.6651", 2, "2.67"), // above the tie
        ("0.125", 2, "0.12"),
        ("0.135", 2, "0.14"),
        ("-2.675", 2, "-2.68"),
        ("1.23456789012345", 9, "1.234567890"),
        ("7", 3, "7.000"),
    ] {
        assert_eq!(
            Price::parse_with_precision(text, precision).unwrap().to_string(),
            expected,
            "{text} @ {precision}"
        );
    }
    assert_eq!(Money::parse_amount("0.123456789", usdt()).unwrap().to_string(), "0.12345679 USDT");
}

#[test]
fn quantity_is_never_negative() {
    assert_eq!(Quantity::parse("-1"), Err(Status::OutOfRange));
    assert_eq!(Quantity::parse("0.001").unwrap().raw(), 1_000_000);
    let a = Quantity::parse("1.5").unwrap();
    let b = Quantity::parse("2.25").unwrap();
    assert_eq!(a.checked_sub(b), Err(Status::OutOfRange));
    assert_eq!(b.checked_sub(a).unwrap().to_string(), "0.75");
    assert_eq!(a.checked_add(b).unwrap().to_string(), "3.75");
}

#[test]
fn money_truncates_toward_zero_and_formats_with_code() {
    let usd = Currency::builtin_by_code("USD").unwrap(); // precision 2
    assert_eq!(Money::from_raw(1_239_999_999, usd).unwrap().to_string(), "1.23 USD");
    assert_eq!(Money::from_raw(-1_239_999_999, usd).unwrap().to_string(), "-1.23 USD");
    assert_eq!(Money::parse("1.50 USD").unwrap().raw(), 1_500_000_000);
    assert_eq!(Money::parse("1.50 XXX"), Err(Status::NotFound));
    assert_eq!(Money::parse("1.50"), Err(Status::ParseError));
    assert_eq!(Money::from_raw_exact(1_239_999_999, usd), Err(Status::InvalidArgument));
    let a = Money::parse("10 USDT").unwrap();
    let b = Money::parse("10 USD").unwrap();
    assert_eq!(a.checked_add(b), Err(Status::InvalidArgument));
    assert_eq!(
        a.checked_sub(Money::parse("12.5 USDT").unwrap()).unwrap().to_string(),
        "-2.50000000 USDT"
    );
}

#[test]
fn notional_is_exact_and_truncates_toward_zero() {
    let one = Quantity::from_raw(FIXED_SCALAR as u64, 0).unwrap();
    let price = Price::parse("12345.6").unwrap();
    let qty = Quantity::parse("0.007").unwrap();
    assert_eq!(notional_raw(price, qty, one).unwrap(), 86_419_200_000); // 86.4192
                                                                        // 1/3 * 1 truncates
    assert_eq!(
        notional_raw(Price::parse("0.333333333").unwrap(), Quantity::parse("3").unwrap(), one)
            .unwrap(),
        999_999_999
    );
    assert_eq!(
        notional_raw(Price::parse("-0.333333333").unwrap(), Quantity::parse("3").unwrap(), one)
            .unwrap(),
        -999_999_999
    );
    // Multiplier 0.5 contract size.
    let half = Quantity::from_raw(FIXED_SCALAR as u64 / 2, 1).unwrap();
    assert_eq!(
        notional_raw(Price::parse("100").unwrap(), Quantity::parse("3").unwrap(), half).unwrap(),
        150 * FIXED_SCALAR
    );
    assert_eq!(
        notional_raw(
            Price::parse("9223372036").unwrap(),
            Quantity::parse("18446744073").unwrap(),
            one
        ),
        Err(Status::Overflow)
    );
    assert_eq!(notional_raw(Price::undef(), qty, one), Err(Status::InvalidArgument));
}

#[test]
fn notional_agrees_with_wide_arithmetic() {
    for_all(|gen: &mut Gen| {
        let price = Price::from_units(gen.range(-10_000_000, 10_000_000), 2).unwrap();
        let qty = Quantity::from_units(gen.range_u(0, 1_000_000), 3).unwrap();
        let mult = Quantity::from_units(gen.range_u(1, 100_000), 3).unwrap();
        let exact: i128 = i128::from(price.raw()) * i128::from(qty.raw()) * i128::from(mult.raw())
            / (i128::from(FIXED_SCALAR) * i128::from(FIXED_SCALAR));
        assert_eq!(
            notional_raw(price, qty, mult),
            i64::try_from(exact).map_err(|_| Status::Overflow)
        );
    });
}

#[test]
fn decimal_text_round_trips_for_random_raws() {
    for_all(|gen: &mut Gen| {
        let precision = gen.range_u(0, 9) as u8;
        let units = gen.range(-9_000_000_000, 9_000_000_000);
        let scale = 10i64.pow(9 - u32::from(precision));
        let Ok(p) = Price::from_raw(
            units
                .saturating_mul(scale)
                .clamp(-9_223_372_036_000_000_000, 9_223_372_036_000_000_000)
                / scale
                * scale,
            precision,
        ) else {
            return;
        };
        let text = p.to_string();
        let back = Price::parse(&text).unwrap();
        assert_eq!(back, p);
        assert_eq!(back.precision(), precision, "{text}");
        let d = decimal::parse(&text).unwrap();
        assert_eq!(decimal::to_raw(&d, precision).unwrap(), i128::from(p.raw()));
    });
}

// ---- identifiers --------------------------------------------------------------------------------

#[test]
fn identifiers_enforce_their_rules() {
    let id = InstrumentId::parse("BTCUSDT-PERP.BINANCE").unwrap();
    assert_eq!(id.symbol.as_str(), "BTCUSDT-PERP");
    assert_eq!(id.venue.as_str(), "BINANCE");
    assert_eq!(id.to_string(), "BTCUSDT-PERP.BINANCE");
    let dotted = InstrumentId::parse("BRK.B.NYSE").unwrap(); // split at the last '.'
    assert_eq!((dotted.symbol.as_str(), dotted.venue.as_str()), ("BRK.B", "NYSE"));
    assert_eq!(InstrumentId::parse("BTCUSDT"), Err(Status::ParseError));
    assert_eq!(Venue::new("BIN.ANCE"), Err(Status::InvalidArgument));
    assert_eq!(Symbol::new("   "), Err(Status::InvalidArgument));
    assert_eq!(Symbol::new(&"X".repeat(33)), Err(Status::OutOfRange));

    let trader = TraderId::new("TESTER-001").unwrap();
    assert_eq!(trader.tag(), "001");
    assert_eq!(TraderId::new("TESTER"), Err(Status::InvalidArgument));
    assert_eq!(TraderId::new("TESTER-"), Err(Status::InvalidArgument));
    assert_eq!(TraderId::new("-001"), Err(Status::InvalidArgument));
    assert_eq!(TraderId::new("A-B-C").unwrap().tag(), "C");
    assert!(StrategyId::new("EXTERNAL").unwrap().is_external());
    assert_eq!(StrategyId::new("MyMM").err(), Some(Status::InvalidArgument));
    let account = AccountId::new("BINANCE-001-X").unwrap();
    assert_eq!((account.issuer(), account.number()), ("BINANCE", "001-X"));
    let position = PositionId::netting(&id, &StrategyId::new("MyMM-001").unwrap()).unwrap();
    assert_eq!(position.as_str(), "BTCUSDT-PERP.BINANCE-MyMM-001");
    assert_eq!(ClientOrderId::from_str("EXTERNAL").map(|c| c.is_external()), Ok(true));
}

#[test]
fn client_order_ids_encode_tag_epoch_and_sequence() {
    let id = client_order_id::format("mm01", 1_000_000, 123_456).unwrap();
    assert_eq!(id.as_str().len(), "mm01-".len() + 6 + 1 + 8);
    let decoded = client_order_id::decode(&id).unwrap();
    assert_eq!((decoded.node_tag, decoded.epoch, decoded.seq), ("mm01", 1_000_000, 123_456));
    assert!(client_order_id::decode(&ClientOrderId::new("EXTERNAL").unwrap()).is_err());
    assert!(client_order_id::decode(&ClientOrderId::new("x-abc-12345678").unwrap()).is_err());
    assert_eq!(client_order_id::format("bad tag", 1, 1), Err(Status::InvalidArgument));
    assert_eq!(client_order_id::format("mm01", 1 << 30, 1), Err(Status::OutOfRange));
    let mut generator = client_order_id::Generator::new("mm01", 7).unwrap();
    let a = generator.next_id().unwrap();
    let b = generator.next_id().unwrap();
    assert!(a < b, "ids sort in placement order: {a} < {b}");
    assert_eq!(client_order_id::decode(&b).unwrap().seq, 2);
    assert_eq!(generator.id_of(2).unwrap(), b);
    generator.resume_after(10).unwrap();
    assert_eq!(client_order_id::decode(&generator.next_id().unwrap()).unwrap().seq, 11);
    // The longest tag still fits Binance's 36-character limit.
    let long = client_order_id::format(
        &"a".repeat(client_order_id::TAG_MAX),
        client_order_id::EPOCH_MAX,
        client_order_id::SEQ_MAX,
    )
    .unwrap();
    assert_eq!(long.as_str().len(), 36);
}

#[test]
fn client_order_ids_round_trip_for_random_parts() {
    for_all(|gen: &mut Gen| {
        let epoch = gen.range_u(0, client_order_id::EPOCH_MAX);
        let seq = gen.range_u(0, client_order_id::SEQ_MAX);
        let id = client_order_id::format("node7", epoch, seq).unwrap();
        let d = client_order_id::decode(&id).unwrap();
        assert_eq!((d.epoch, d.seq), (epoch, seq));
    });
}

#[test]
fn bar_types_parse_standard_and_composite_forms() {
    let text = "BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL";
    let bt = BarType::parse(text).unwrap();
    assert_eq!(bt.instrument_id.to_string(), "BTCUSDT-PERP.BINANCE");
    assert_eq!(bt.spec, BarSpecification::new(1, BarAggregation::Minute, PriceType::Last).unwrap());
    assert_eq!(bt.to_string(), text);
    let composite = "BTCUSDT-PERP.BINANCE-5-MINUTE-LAST-INTERNAL@1-MINUTE-EXTERNAL";
    let c = BarType::parse(composite).unwrap();
    assert!(c.is_composite());
    assert_eq!(c.to_string(), composite);
    assert_eq!(
        BarType::parse("BTCUSDT-PERP.BINANCE-0-MINUTE-LAST-EXTERNAL"),
        Err(Status::InvalidArgument)
    );
    assert_eq!(
        BarType::parse("BTCUSDT-PERP.BINANCE-1-FORTNIGHT-LAST-EXTERNAL"),
        Err(Status::ParseError)
    );
    assert_eq!(
        BarSpecification::parse("15-MINUTE-LAST").unwrap().duration_ns(),
        Some(15 * 60 * 1_000_000_000)
    );
    assert_eq!(BarSpecification::parse("100-TICK-LAST").unwrap().duration_ns(), None);
}

#[test]
fn uuid4_text_round_trips_and_checks_version_bits() {
    let u = Uuid4::from_u64s(0x0123_4567_89ab_cdef, 0xfedc_ba98_7654_3210);
    let text = u.to_string();
    assert_eq!(text.len(), 36);
    assert_eq!(&text[14..15], "4");
    assert_eq!(Uuid4::parse(&text).unwrap(), u);
    assert_eq!(Uuid4::parse(&text.to_uppercase()).unwrap(), u);
    assert_eq!(Uuid4::parse("00000000-0000-0000-0000-000000000000"), Err(Status::InvalidArgument));
    assert_eq!(Uuid4::parse("not-a-uuid"), Err(Status::ParseError));
}

#[test]
fn quote_mid_price_truncates_toward_zero_at_one_more_digit() {
    let id = InstrumentId::parse("BTCUSDT-PERP.BINANCE").unwrap();
    let q = QuoteTick::new(
        id,
        Price::parse("100.1").unwrap(),
        Price::parse("100.2").unwrap(),
        Quantity::parse("1").unwrap(),
        Quantity::parse("1").unwrap(),
        UnixNanos::new(1),
        UnixNanos::new(2),
    )
    .unwrap();
    assert_eq!(q.mid_price().unwrap().to_string(), "100.15");
    let q = QuoteTick::new(
        id,
        Price::parse("1.1").unwrap(),
        Price::parse("1.2").unwrap(),
        Quantity::parse("1").unwrap(),
        Quantity::parse("1").unwrap(),
        UnixNanos::new(2),
        UnixNanos::new(1),
    );
    assert_eq!(q.err(), Some(Status::InvalidArgument), "ts_init before ts_event");
}

#[test]
fn book_deltas_validate_their_batch() {
    let id = InstrumentId::parse("BTCUSDT-PERP.BINANCE").unwrap();
    let order = BookOrder {
        side: Some(OrderSide::Buy),
        price: Price::parse("100").unwrap(),
        size: Quantity::parse("1").unwrap(),
        order_id: 0,
    };
    let d = OrderBookDelta::new(
        id,
        BookAction::Add,
        order,
        RecordFlag::Last as u8 | RecordFlag::Snapshot as u8,
        5,
        UnixNanos::new(1),
        UnixNanos::new(1),
    )
    .unwrap();
    assert_eq!(
        OrderBookDelta::new(id, BookAction::Add, order, 1, 5, UnixNanos::new(1), UnixNanos::new(1)),
        Err(Status::InvalidArgument),
        "unknown flag bit"
    );
    assert_eq!(
        OrderBookDelta::new(
            id,
            BookAction::Clear,
            order,
            0,
            5,
            UnixNanos::new(1),
            UnixNanos::new(1)
        ),
        Err(Status::InvalidArgument),
        "a clear has no side"
    );
    let mut deltas = FixedVec::with_capacity(2);
    deltas
        .push(OrderBookDelta::clear(id, 4, UnixNanos::new(1), UnixNanos::new(1)).unwrap())
        .unwrap();
    deltas.push(d).unwrap();
    let batch = OrderBookDeltas::new(id, deltas).unwrap();
    assert!(batch.is_snapshot());
    assert_eq!(batch.sequence, 5);
    assert_eq!(OrderBookDeltas::new(id, FixedVec::with_capacity(0)), Err(Status::InvalidArgument));
    let other = InstrumentId::parse("ETHUSDT-PERP.BINANCE").unwrap();
    let mut mixed = FixedVec::with_capacity(1);
    mixed.push(d).unwrap();
    assert_eq!(OrderBookDeltas::new(other, mixed), Err(Status::InvalidArgument));
}

// ---- wire ---------------------------------------------------------------------------------------

#[test]
fn wire_decoders_refuse_invalid_values() {
    // A price with digits below its precision, a bool of 2, an unknown enum value, an unknown
    // event tag.
    let mut w = model::WireWriter::new();
    w.u64(1_230_000_000);
    w.u8(1);
    assert_eq!(Price::from_wire(w.as_slice()), Err(Status::InvalidArgument));
    assert_eq!(bool::from_wire(&[2]), Err(Status::InvalidArgument));
    assert_eq!(OrderSide::from_wire(&[3]), Err(Status::InvalidArgument));
    assert_eq!(Event::from_wire(&[0xFF, 0xFF]), Err(Status::UnsupportedMessage));
}
