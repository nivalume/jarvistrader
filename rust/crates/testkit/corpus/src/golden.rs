//! The golden catalog (the data of `tests/golden/replay_*`, `tests/cpp/golden_node.cpp` in the
//! C++ tree): ten minutes of `BTCUSDT-PERP.BINANCE` on 2026-09-01 with a random-walk mid, as
//! `aggTrade`, `bookTicker`, `depth` and `kline_1m` streams, and the instrument definition the
//! simulated venue needs. The same seed produces the same events in every build.

use alloc::string::ToString;
use alloc::vec::Vec;

use kernel_core::{EventKey, FixedVec, Result, UnixNanos};
use model::bar::{Bar, BarType};
use model::data::{BookOrder, OrderBookDelta, OrderBookDeltas, QuoteTick, TradeTick};
use model::enums::{AggressorSide, AssetClass, BookAction, OrderSide, RecordFlag};
use model::identifiers::{InstrumentId, Symbol, TradeId};
use model::instruments::{Instrument, InstrumentKind, InstrumentLimits};
use model::{Currency, Event, Price, Quantity};

extern crate alloc;

/// 2026-09-01T00:00:00Z.
pub const DAY: u64 = 1_788_220_800_000_000_000;
/// The catalog's day directory.
pub const DAY_NAME: &str = "2026-09-01";
pub const INSTRUMENT: &str = "BTCUSDT-PERP.BINANCE";
const SECOND: u64 = 1_000_000_000;

/// The streams of the catalog, with the source id each stream's keys carry.
pub const STREAMS: [(&str, u16); 4] =
    [("aggTrade", 1), ("bookTicker", 2), ("depth", 3), ("kline_1m", 4)];
pub const INSTRUMENT_STREAM: (&str, u16) = ("instrument", 9);

#[must_use]
pub fn instrument_id() -> InstrumentId {
    InstrumentId::parse(INSTRUMENT).expect("instrument id")
}

fn px(tenths: i64) -> Price {
    Price::from_raw(tenths * 100_000_000, 1).expect("price")
}

fn qty(thousandths: u64) -> Quantity {
    Quantity::from_raw(thousandths * 1_000_000, 3).expect("quantity")
}

/// `splitmix64`: the catalog's own generator, shared with the C++ tree.
#[derive(Clone, Copy, Debug)]
pub struct SplitMix {
    state: u64,
}

impl SplitMix {
    #[must_use]
    pub const fn new(seed: u64) -> Self {
        Self { state: seed }
    }
    pub fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    pub fn below(&mut self, n: u64) -> u64 {
        self.next_u64() % n
    }
}

/// The BTCUSDT perpetual as Binance defines it (tick 0.1, step 0.001), dated at the day's start.
pub fn instrument() -> Result<Instrument> {
    let usdt = Currency::builtin_by_code("USDT").expect("USDT");
    let btc = Currency::builtin_by_code("BTC").expect("BTC");
    Instrument::new(
        instrument_id(),
        Symbol::new("BTCUSDT")?,
        AssetClass::Cryptocurrency,
        InstrumentKind::CryptoPerpetual { settlement_currency: usdt, is_inverse: false },
        Some(btc),
        usdt,
        px(1),
        qty(1),
        Quantity::from_units(1, 0)?,
        None,
        InstrumentLimits::default(),
        Quantity::parse("0.05")?,
        Quantity::parse("0.025")?,
        UnixNanos::new(DAY),
        UnixNanos::new(DAY),
    )
}

/// One stream's records, in key order.
#[derive(Clone, Debug, Default)]
pub struct Stream {
    pub name: &'static str,
    pub records: Vec<(EventKey, Event)>,
}

fn delta(
    bid: bool,
    action: BookAction,
    tenths: i64,
    size: u64,
    flags: u8,
    seq: u64,
    ts: u64,
) -> Result<OrderBookDelta> {
    OrderBookDelta::new(
        instrument_id(),
        action,
        BookOrder {
            side: Some(if bid { OrderSide::Buy } else { OrderSide::Sell }),
            price: px(tenths),
            size: qty(size),
            order_id: 0,
        },
        flags,
        seq,
        UnixNanos::new(ts),
        UnixNanos::new(ts),
    )
}

fn deltas(items: Vec<OrderBookDelta>) -> Result<Event> {
    let mut v = FixedVec::with_capacity(items.len());
    for d in items {
        v.push(d)?;
    }
    Ok(Event::OrderBookDeltas(OrderBookDeltas::new(instrument_id(), v)?))
}

/// The four market data streams for `seed`, in the order of [`STREAMS`].
#[allow(clippy::too_many_lines)]
pub fn catalog(seed: u64) -> Result<[Stream; 4]> {
    let mut rng = SplitMix::new(seed);
    let id = instrument_id();
    let mut trades = Stream { name: "aggTrade", records: Vec::new() };
    let mut quotes = Stream { name: "bookTicker", records: Vec::new() };
    let mut depth = Stream { name: "depth", records: Vec::new() };
    let mut klines = Stream { name: "kline_1m", records: Vec::new() };
    let mut mid: i64 = 650_000; // tenths
    let mut trade_seq = 0u64;
    let mut quote_seq = 0u64;
    let mut depth_seq = 0u64;
    let snapshot_flag = RecordFlag::Snapshot.value();
    let last_flag = RecordFlag::Last.value();

    // A snapshot of five levels per side, then updates.
    let mut batch = Vec::new();
    for level in 1..=5i64 {
        for bid in [true, false] {
            let flags = if level == 5 && !bid { snapshot_flag | last_flag } else { snapshot_flag };
            batch.push(delta(
                bid,
                BookAction::Add,
                if bid { mid - level } else { mid + level },
                1000 + level as u64 * 100,
                flags,
                1,
                DAY + 1,
            )?);
        }
    }
    depth_seq += 1;
    depth.records.push((EventKey::new(UnixNanos::new(DAY + 1), 3, depth_seq), deltas(batch)?));

    let bar_type = BarType::parse("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL")?;
    for step in 1..=600u64 {
        let ts = DAY + step * SECOND;
        let t = UnixNanos::new(ts);
        mid += rng.below(5) as i64 - 2;
        if step % 60 == 0 {
            let bar = Bar::new(
                bar_type,
                px(mid - 3),
                px(mid + 4),
                px(mid - 5),
                px(mid),
                qty(12_345),
                t,
                t,
            )?;
            klines.records.push((EventKey::new(t, 4, step / 60), Event::Bar(bar)));
        }
        if step % 2 == 0 {
            let bid_size = 500 + rng.below(1500);
            let ask_size = 500 + rng.below(1500);
            let q =
                QuoteTick::new(id, px(mid - 1), px(mid + 1), qty(bid_size), qty(ask_size), t, t)?;
            quote_seq += 1;
            quotes.records.push((EventKey::new(t, 2, quote_seq), Event::QuoteTick(q)));
        }
        let mut k = rng.below(3);
        while k > 0 {
            let offset = rng.below(3) as i64 - 1;
            let size = 1 + rng.below(900);
            let buy = rng.below(2) == 0;
            let trade_id = TradeId::new(&(trade_seq + 1).to_string())?;
            let tt = TradeTick::new(
                id,
                px(mid + offset),
                qty(size),
                if buy { AggressorSide::Buy } else { AggressorSide::Sell },
                trade_id,
                t,
                t,
            )?;
            trade_seq += 1;
            trades.records.push((EventKey::new(t, 1, trade_seq), Event::TradeTick(tt)));
            k -= 1;
        }
        if step % 5 == 0 {
            let mut batch = Vec::new();
            for bid in [true, false] {
                let size = 100 + rng.below(2000);
                let flags = if bid { 0 } else { last_flag };
                batch.push(delta(
                    bid,
                    BookAction::Update,
                    if bid { mid - 1 } else { mid + 1 },
                    size,
                    flags,
                    1 + step,
                    ts,
                )?);
            }
            depth_seq += 1;
            depth.records.push((EventKey::new(t, 3, depth_seq), deltas(batch)?));
        }
    }
    Ok([trades, quotes, depth, klines])
}

/// The `instrument` stream: one definition at the day's start.
pub fn instrument_stream() -> Result<Stream> {
    Ok(Stream {
        name: INSTRUMENT_STREAM.0,
        records: alloc::vec![(
            EventKey::new(UnixNanos::new(DAY), INSTRUMENT_STREAM.1, 1),
            Event::Instrument(instrument()?)
        )],
    })
}
