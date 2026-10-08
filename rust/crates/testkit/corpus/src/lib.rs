//! A deterministic corpus of kernel inputs: every event kind, every order event variant, with
//! field values drawn from a counter-based generator keyed by the seed and the record number.
//!
//! Test support, not kernel code (docs/rust-plan.md): it drives the encoding property tests, seeds
//! the fuzzers, and is what the determinism gate fingerprints in two build profiles
//! (`jarvis-rs corpus`). Two builds must produce the same corpus byte for byte.
//!
//! Every value goes through the type's validating constructor; a draw that lands on an invalid
//! value is adjusted, never skipped, so the record count is exactly what was asked for.
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

use kernel_core::clock::TimerKey;
use kernel_core::rng::CounterRng;
use kernel_core::{DurationNanos, EventKey, FixedString, FixedVec, Result, Status, UnixNanos};

use model::account::{AccountBalance, AccountState, MarginBalance};
use model::bar::{Bar, BarSpecification, BarType, CompositeSource};
use model::client_order_id;
use model::currency::Currency;
use model::data::{
    BookOrder, FundingRateUpdate, IndexPriceUpdate, InstrumentClose, InstrumentStatus,
    LiquidationOrder, MarkPriceUpdate, OrderBookDelta, OrderBookDeltas, QuoteTick, TradeTick,
};
use model::enums::{
    AccountType, AggregationSource, AggressorSide, AssetClass, BarAggregation, BookAction,
    InstrumentCloseType, LiquiditySide, MarketStatusAction, OrderSide, OrderType, PriceType,
    RecordFlag, StopMode, TimeInForce, TriggerType,
};
use model::event::{BatchEnd, Event, EventKind, RateLimitFeedback, Shutdown, TimerFired};
use model::fixed_point::{Price, Quantity, FIXED_SCALAR};
use model::identifiers::{
    AccountId, ClientOrderId, InstrumentId, PositionId, StrategyId, Symbol, TradeId, TraderId,
    Venue, VenueOrderId,
};
use model::instruments::{Instrument, InstrumentKind, InstrumentLimits};
use model::money::Money;
use model::order_events::{
    FillInfoFlags, OrderAccepted, OrderCancelRejected, OrderCanceled, OrderDenied, OrderEmulated,
    OrderEvent, OrderEventHeader, OrderExpired, OrderFillVoided, OrderFilled, OrderInitialized,
    OrderModifyRejected, OrderPendingCancel, OrderPendingUpdate, OrderRejected, OrderReleased,
    OrderSubmitted, OrderTriggered, OrderUpdated,
};
use model::uuid::Uuid4;

/// The generator. `hop` numbers in [`Draw`] name what a draw is for, so adding a field never shifts
/// another field's value.
#[derive(Clone, Copy, Debug)]
pub struct Corpus {
    rng: CounterRng,
    seed: u64,
}

#[repr(u32)]
#[derive(Clone, Copy)]
enum Draw {
    Kind = 1,
    Ts = 2,
    Latency = 3,
    Price = 4,
    Size = 5,
    Enum = 7,
    Id = 8,
    Flags = 9,
    Count = 10,
    Uuid = 11,
    Option = 13,
    Money = 14,
}

const SYMBOLS: &[&str] =
    &["BTCUSDT-PERP", "ETHUSDT-PERP", "SOLUSDT-PERP", "BTCUSDT", "BTCUSDT_241227"];
const REASONS: &[&str] = &[
    "NOTIONAL_EXCEEDS_MAX_PER_ORDER",
    "RATE_LIMIT_EXCEEDED",
    "TRADING_HALTED",
    "PRICE_FILTER",
    "Unknown order sent.",
];

impl Corpus {
    #[must_use]
    pub const fn new(seed: u64) -> Self {
        Self { rng: CounterRng::new(seed), seed }
    }

    #[must_use]
    pub const fn seed(&self) -> u64 {
        self.seed
    }

    /// Record `i` (from 1): its key and event. Keys are strictly increasing in `(ts, seq)`.
    pub fn record(&self, i: u64) -> Result<(EventKey, Event)> {
        // Records are 10 ms apart and arrive within 5 ms, so `ts_init` strictly increases and the
        // keys are in the order a merged backtest source would deliver them.
        let ts_event = UnixNanos::new(
            1_700_000_000_000_000_000 + i * 10_000_000 + self.below(4_000_000, i, Draw::Ts, 0),
        );
        let latency = DurationNanos::new(self.below(5_000_000, i, Draw::Latency, 0));
        let ts_init = ts_event.plus(latency)?;
        let kind =
            EventKind::ALL[self.below(EventKind::ALL.len() as u64, i, Draw::Kind, 0) as usize];
        let event = self.event(kind, i, ts_event, ts_init)?;
        let source_id =
            if matches!(kind, EventKind::TimerFired | EventKind::BatchEnd | EventKind::Shutdown) {
                0
            } else {
                1 + self.below(3, i, Draw::Id, 9) as u16
            };
        Ok((EventKey::new(ts_init, source_id, i), event))
    }

    /// Records 1 to `count`.
    pub fn records(&self, count: u64) -> impl Iterator<Item = Result<(EventKey, Event)>> + '_ {
        (1..=count).map(move |i| self.record(i))
    }

    // ---- draws --------------------------------------------------------------------------------

    fn below(&self, bound: u64, i: u64, hop: Draw, index: u32) -> u64 {
        self.rng.below(bound, i, hop as u32, index)
    }
    fn chance(&self, numerator: u64, denominator: u64, i: u64, index: u32) -> bool {
        self.below(denominator, i, Draw::Option, index) < numerator
    }
    fn pick<T: Copy>(&self, items: &[T], i: u64, index: u32) -> T {
        items[self.below(items.len() as u64, i, Draw::Enum, index) as usize]
    }
    fn uuid(&self, i: u64, index: u32) -> Uuid4 {
        Uuid4::from_u64s(
            self.rng.draw(i, Draw::Uuid as u32, index * 2),
            self.rng.draw(i, Draw::Uuid as u32, index * 2 + 1),
        )
    }
    fn price(&self, i: u64, index: u32) -> Result<Price> {
        // 1.00 to 100_000.00 at precision 1 or 2, plus a few at precision 8 for small coins.
        let precision = [1u8, 2, 2, 2, 8][self.below(5, i, Draw::Price, index * 2) as usize];
        let units = 100 + self.below(10_000_000, i, Draw::Price, index * 2 + 1);
        Price::from_units(units as i64, precision)
    }
    fn size(&self, i: u64, index: u32) -> Result<Quantity> {
        let precision = [3u8, 3, 0, 1][self.below(4, i, Draw::Size, index * 2) as usize];
        let units = 1 + self.below(50_000, i, Draw::Size, index * 2 + 1);
        Quantity::from_units(units, precision)
    }
    fn money(&self, i: u64, index: u32, currency: Currency) -> Result<Money> {
        let units = self.below(5_000_000_000, i, Draw::Money, index) as i64 - 1_000_000_000;
        Money::from_raw(units.checked_mul(1_000).ok_or(Status::Overflow)?, currency)
    }
    fn instrument_id(&self, i: u64, index: u32) -> Result<InstrumentId> {
        Ok(InstrumentId::new(
            Symbol::new(self.pick(SYMBOLS, i, 100 + index))?,
            Venue::new("BINANCE")?,
        ))
    }
    fn trade_id(&self, i: u64, index: u32) -> Result<TradeId> {
        let n = self.rng.draw(i, Draw::Id as u32, index);
        let mut buf = [0u8; 20];
        let len = write_decimal(n, &mut buf);
        TradeId::new(core::str::from_utf8(&buf[..len]).map_err(|_| Status::InvalidArgument)?)
    }
    fn venue_order_id(&self, i: u64, index: u32) -> Result<VenueOrderId> {
        let n = self.rng.draw(i, Draw::Id as u32, index) % 10_000_000_000;
        let mut buf = [0u8; 20];
        let len = write_decimal(n, &mut buf);
        VenueOrderId::new(core::str::from_utf8(&buf[..len]).map_err(|_| Status::InvalidArgument)?)
    }
    fn client_order_id(&self, i: u64, index: u32) -> Result<ClientOrderId> {
        if self.chance(1, 8, i, 50 + index) {
            return ClientOrderId::new("EXTERNAL");
        }
        client_order_id::format(
            "mm01",
            self.below(1 << 30, i, Draw::Id, 20 + index),
            1 + self.below(1 << 20, i, Draw::Id, 40 + index),
        )
    }
    fn reason(&self, i: u64, index: u32) -> Result<FixedString<96>> {
        FixedString::from_text(self.pick(REASONS, i, 200 + index))
    }
    fn usdt() -> Currency {
        Currency::default()
    }
    fn btc() -> Currency {
        Currency::builtin_by_code("BTC").unwrap_or_default()
    }

    // ---- events -------------------------------------------------------------------------------

    #[allow(clippy::too_many_lines)] // one arm per event kind; splitting it hides the coverage
    fn event(
        &self,
        kind: EventKind,
        i: u64,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Event> {
        Ok(match kind {
            EventKind::TradeTick => Event::TradeTick(TradeTick::new(
                self.instrument_id(i, 0)?,
                self.price(i, 0)?,
                self.size(i, 0)?,
                self.pick(AggressorSide::ALL, i, 0),
                self.trade_id(i, 0)?,
                ts_event,
                ts_init,
            )?),
            EventKind::QuoteTick => {
                let bid = self.price(i, 0)?;
                let ask = bid.checked_add(Price::from_units(
                    1 + self.below(50, i, Draw::Price, 9) as i64,
                    bid.precision(),
                )?)?;
                Event::QuoteTick(QuoteTick::new(
                    self.instrument_id(i, 0)?,
                    bid,
                    ask,
                    self.size(i, 0)?,
                    self.size(i, 1)?,
                    ts_event,
                    ts_init,
                )?)
            }
            EventKind::OrderBookDeltas => self.book_deltas(i, ts_event, ts_init)?,
            EventKind::Bar => {
                let a = self.price(i, 0)?;
                let b = self.price(i, 1)?;
                let (low, high) = if a <= b { (a, b) } else { (b, a) };
                let open = if self.chance(1, 2, i, 1) { low } else { high };
                let close = if self.chance(1, 2, i, 2) { high } else { low };
                let spec = BarSpecification::new(
                    1 + self.below(60, i, Draw::Count, 0) as u32,
                    self.pick(
                        &[
                            BarAggregation::Second,
                            BarAggregation::Minute,
                            BarAggregation::Hour,
                            BarAggregation::Tick,
                            BarAggregation::Volume,
                        ],
                        i,
                        1,
                    ),
                    self.pick(
                        &[PriceType::Last, PriceType::Mid, PriceType::Bid, PriceType::Ask],
                        i,
                        2,
                    ),
                )?;
                let mut bar_type = BarType::standard(
                    self.instrument_id(i, 0)?,
                    spec,
                    self.pick(AggregationSource::ALL, i, 3),
                );
                if self.chance(1, 5, i, 3) {
                    bar_type.composite = Some(CompositeSource {
                        step: 1,
                        aggregation: BarAggregation::Minute,
                        aggregation_source: AggregationSource::External,
                    });
                }
                Event::Bar(Bar::new(
                    bar_type,
                    open,
                    high,
                    low,
                    close,
                    self.size(i, 0)?,
                    ts_event,
                    ts_init,
                )?)
            }
            EventKind::MarkPriceUpdate => Event::MarkPriceUpdate(MarkPriceUpdate::new(
                self.instrument_id(i, 0)?,
                self.price(i, 0)?,
                ts_event,
                ts_init,
            )?),
            EventKind::IndexPriceUpdate => Event::IndexPriceUpdate(IndexPriceUpdate::new(
                self.instrument_id(i, 0)?,
                self.price(i, 0)?,
                ts_event,
                ts_init,
            )?),
            EventKind::FundingRateUpdate => {
                let rate = Price::from_raw(
                    (self.below(2_000_000, i, Draw::Price, 0) as i64 - 1_000_000) * 10,
                    8,
                )?; // +-0.001 at 8 digits
                let interval = if self.chance(3, 4, i, 1) { Some(480) } else { None };
                let next = if self.chance(3, 4, i, 2) {
                    Some(ts_event.plus(DurationNanos::new(8 * 3600 * 1_000_000_000))?)
                } else {
                    None
                };
                Event::FundingRateUpdate(FundingRateUpdate::new(
                    self.instrument_id(i, 0)?,
                    rate,
                    interval,
                    next,
                    ts_event,
                    ts_init,
                )?)
            }
            EventKind::InstrumentStatus => Event::InstrumentStatus(InstrumentStatus {
                instrument_id: self.instrument_id(i, 0)?,
                action: self.pick(MarketStatusAction::ALL, i, 0),
                ts_event,
                ts_init,
                reason: if self.chance(1, 2, i, 1) {
                    Some(FixedString::from_text("scheduled")?)
                } else {
                    None
                },
                trading_event: None,
                is_trading: if self.chance(1, 2, i, 2) {
                    Some(self.chance(1, 2, i, 3))
                } else {
                    None
                },
                is_quoting: None,
                is_short_sell_restricted: None,
            }),
            EventKind::InstrumentClose => Event::InstrumentClose(InstrumentClose::new(
                self.instrument_id(i, 0)?,
                self.price(i, 0)?,
                self.pick(InstrumentCloseType::ALL, i, 0),
                ts_event,
                ts_init,
            )?),
            EventKind::LiquidationOrder => {
                let original = self.size(i, 0)?;
                let filled = if self.chance(1, 2, i, 1) {
                    original
                } else {
                    Quantity::from_units(original.units() / 2, original.precision())?
                };
                Event::LiquidationOrder(LiquidationOrder::new(
                    self.instrument_id(i, 0)?,
                    self.pick(OrderSide::ALL, i, 0),
                    self.price(i, 0)?,
                    self.price(i, 1)?,
                    original,
                    filled,
                    ts_event,
                    ts_init,
                )?)
            }
            EventKind::Instrument => self.instrument(i, ts_event, ts_init)?,
            EventKind::Order => Event::Order(self.order_event(i, ts_event, ts_init)?),
            EventKind::AccountState => self.account_state(i, ts_event, ts_init)?,
            EventKind::RateLimitFeedback => Event::RateLimitFeedback(RateLimitFeedback {
                used_weight_1m: self.below(2400, i, Draw::Count, 0) as u32,
                order_count_10s: self.below(300, i, Draw::Count, 1) as u32,
                order_count_1m: self.below(1200, i, Draw::Count, 2) as u32,
                ts_init,
            }),
            EventKind::TimerFired => Event::TimerFired(TimerFired {
                key: TimerKey::new(
                    self.below(4, i, Draw::Id, 0) as u32,
                    self.below(16, i, Draw::Id, 1) as u32,
                ),
                deadline: ts_init,
            }),
            EventKind::BatchEnd => Event::BatchEnd(BatchEnd { ts: ts_init }),
            EventKind::Shutdown => {
                Event::Shutdown(Shutdown { mode: self.pick(StopMode::ALL, i, 0) })
            }
        })
    }

    fn book_deltas(&self, i: u64, ts_event: UnixNanos, ts_init: UnixNanos) -> Result<Event> {
        let instrument_id = self.instrument_id(i, 0)?;
        let snapshot = self.chance(1, 4, i, 0);
        let count = 1 + self.below(if snapshot { 20 } else { 6 }, i, Draw::Count, 0) as usize;
        let mut deltas = FixedVec::with_capacity(count + usize::from(snapshot));
        let sequence = self.rng.draw(i, Draw::Id as u32, 0) % 1_000_000_000_000;
        if snapshot {
            deltas.push(OrderBookDelta::clear(instrument_id, sequence, ts_event, ts_init)?)?;
        }
        for k in 0..count {
            let last = k + 1 == count;
            let mut flags = if last { RecordFlag::Last as u8 } else { 0 };
            if snapshot && last {
                flags |= RecordFlag::Snapshot as u8;
            }
            if self.chance(1, 3, i, 10 + k as u32) {
                flags |= RecordFlag::Mbp as u8;
            }
            let action = if snapshot {
                BookAction::Add
            } else {
                self.pick(
                    &[BookAction::Add, BookAction::Update, BookAction::Delete],
                    i,
                    10 + k as u32,
                )
            };
            let order = BookOrder {
                side: Some(self.pick(OrderSide::ALL, i, 40 + k as u32)),
                price: self.price(i, 10 + k as u32)?,
                size: if action == BookAction::Delete {
                    Quantity::zero(3)
                } else {
                    self.size(i, 10 + k as u32)?
                },
                order_id: 0,
            };
            deltas.push(OrderBookDelta::new(
                instrument_id,
                action,
                order,
                flags,
                sequence + k as u64 + 1,
                ts_event,
                ts_init,
            )?)?;
        }
        Ok(Event::OrderBookDeltas(OrderBookDeltas::new(instrument_id, deltas)?))
    }

    fn instrument(&self, i: u64, ts_event: UnixNanos, ts_init: UnixNanos) -> Result<Event> {
        let id = self.instrument_id(i, 0)?;
        let symbol = id.symbol.as_str();
        let kind = if symbol.contains("-PERP") {
            InstrumentKind::CryptoPerpetual { settlement_currency: Self::usdt(), is_inverse: false }
        } else if symbol.contains('_') {
            InstrumentKind::CryptoFuture {
                underlying: Self::btc(),
                settlement_currency: Self::usdt(),
                is_inverse: false,
                activation_ns: UnixNanos::new(1_690_000_000_000_000_000),
                expiration_ns: UnixNanos::new(1_735_000_000_000_000_000),
            }
        } else {
            InstrumentKind::CurrencyPair
        };
        let price_increment = Price::from_units(1, self.pick(&[1u8, 2, 2, 4], i, 0))?;
        let size_increment = Quantity::from_units(1, self.pick(&[3u8, 3, 1, 0], i, 1))?;
        let limits = InstrumentLimits {
            max_quantity: Some(Quantity::from_units(1000, 0)?),
            min_quantity: Some(size_increment),
            max_notional: None,
            min_notional: if self.chance(1, 2, i, 2) {
                Some(Money::from_raw(100 * FIXED_SCALAR, Self::usdt())?)
            } else {
                None
            },
            max_price: Some(Price::from_units(1_000_000, 0)?),
            min_price: Some(price_increment),
        };
        Ok(Event::Instrument(Instrument::new(
            id,
            Symbol::new(symbol.split('-').next().unwrap_or(symbol))?,
            AssetClass::Cryptocurrency,
            kind,
            Some(Self::btc()),
            Self::usdt(),
            price_increment,
            size_increment,
            Quantity::from_raw(FIXED_SCALAR as u64, 0)?,
            Some(size_increment),
            limits,
            Quantity::from_raw(
                FIXED_SCALAR as u64 / 100 * (1 + self.below(10, i, Draw::Count, 3)),
                2,
            )?,
            Quantity::from_raw(
                FIXED_SCALAR as u64 / 1000 * (1 + self.below(50, i, Draw::Count, 4)),
                3,
            )?,
            ts_event,
            ts_init,
        )?))
    }

    fn account_state(&self, i: u64, ts_event: UnixNanos, ts_init: UnixNanos) -> Result<Event> {
        let mut balances = FixedVec::with_capacity(2);
        let usdt = Self::usdt();
        let free = Money::from_raw(
            self.below(1_000_000, i, Draw::Money, 0) as i64 * FIXED_SCALAR / 1000,
            usdt,
        )?;
        let locked = Money::from_raw(
            self.below(100_000, i, Draw::Money, 1) as i64 * FIXED_SCALAR / 1000,
            usdt,
        )?;
        balances.push(AccountBalance::new(free.checked_add(locked)?, locked, free)?)?;
        if self.chance(1, 3, i, 0) {
            let bnb = Currency::builtin_by_code("BNB").unwrap_or_default();
            let free = Money::from_raw(
                self.below(10_000, i, Draw::Money, 2) as i64 * FIXED_SCALAR / 100,
                bnb,
            )?;
            balances.push(AccountBalance::new(free, Money::zero(bnb), free)?)?;
        }
        let mut margins = FixedVec::with_capacity(1);
        if self.chance(1, 2, i, 1) {
            margins.push(MarginBalance::new(
                Money::from_raw(
                    self.below(50_000, i, Draw::Money, 3) as i64 * FIXED_SCALAR / 1000,
                    usdt,
                )?,
                Money::from_raw(
                    self.below(20_000, i, Draw::Money, 4) as i64 * FIXED_SCALAR / 1000,
                    usdt,
                )?,
                Some(self.instrument_id(i, 1)?),
            )?)?;
        }
        Ok(Event::AccountState(AccountState::new(
            AccountId::new("BINANCE-001")?,
            AccountType::Margin,
            Some(usdt),
            balances,
            margins,
            true,
            self.uuid(i, 0),
            ts_event,
            ts_init,
        )?))
    }

    fn order_header(
        &self,
        i: u64,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<OrderEventHeader> {
        Ok(OrderEventHeader {
            trader_id: TraderId::new("TESTER-001")?,
            strategy_id: StrategyId::new(self.pick(
                &["MyMM-001", "Trend-002", "EXTERNAL"],
                i,
                300,
            ))?,
            instrument_id: self.instrument_id(i, 0)?,
            client_order_id: self.client_order_id(i, 0)?,
            event_id: self.uuid(i, 0),
            ts_event,
            ts_init,
            causation_id: if self.chance(1, 2, i, 400) { Some(self.uuid(i, 1)) } else { None },
        })
    }

    #[allow(clippy::too_many_lines)] // one arm per order event variant
    fn order_event(&self, i: u64, ts_event: UnixNanos, ts_init: UnixNanos) -> Result<OrderEvent> {
        let header = self.order_header(i, ts_event, ts_init)?;
        let account_id = AccountId::new("BINANCE-001")?;
        let reconciliation = self.chance(1, 10, i, 401);
        let opt_venue =
            if self.chance(3, 4, i, 402) { Some(self.venue_order_id(i, 1)?) } else { None };
        let opt_account = if self.chance(3, 4, i, 403) { Some(account_id) } else { None };
        let variant = self.below(17, i, Draw::Kind, 1);
        Ok(match variant {
            0 => {
                let order_type = self.pick(
                    &[
                        OrderType::Limit,
                        OrderType::Market,
                        OrderType::StopMarket,
                        OrderType::StopLimit,
                    ],
                    i,
                    410,
                );
                let needs_price = matches!(order_type, OrderType::Limit | OrderType::StopLimit);
                let needs_trigger =
                    matches!(order_type, OrderType::StopMarket | OrderType::StopLimit);
                let tif = if order_type == OrderType::Market {
                    TimeInForce::Ioc
                } else {
                    self.pick(
                        &[TimeInForce::Gtc, TimeInForce::Ioc, TimeInForce::Fok, TimeInForce::Gtd],
                        i,
                        411,
                    )
                };
                OrderEvent::Initialized(OrderInitialized {
                    header,
                    order_side: self.pick(OrderSide::ALL, i, 412),
                    order_type,
                    quantity: self.size(i, 0)?,
                    time_in_force: tif,
                    post_only: needs_price && self.chance(1, 2, i, 413),
                    reduce_only: self.chance(1, 5, i, 414),
                    quote_quantity: false,
                    reconciliation,
                    price: if needs_price { Some(self.price(i, 0)?) } else { None },
                    activation_price: None,
                    trigger_price: if needs_trigger { Some(self.price(i, 1)?) } else { None },
                    trigger_type: if needs_trigger {
                        Some(self.pick(&[TriggerType::LastPrice, TriggerType::MarkPrice], i, 415))
                    } else {
                        None
                    },
                    limit_offset: None,
                    trailing_offset: None,
                    trailing_offset_type: None,
                    expire_time: if tif == TimeInForce::Gtd {
                        Some(ts_event.plus(DurationNanos::new(3_600_000_000_000))?)
                    } else {
                        None
                    },
                    display_qty: None,
                    emulation_trigger: None,
                    trigger_instrument_id: None,
                    contingency_type: None,
                    order_list_id: None,
                    parent_order_id: None,
                    exec_algorithm_id: None,
                    exec_spawn_id: if self.chance(1, 4, i, 416) {
                        Some(self.client_order_id(i, 1)?)
                    } else {
                        None
                    },
                })
            }
            1 => OrderEvent::Denied(OrderDenied { header, reason: self.reason(i, 0)? }),
            2 => OrderEvent::Emulated(OrderEmulated { header }),
            3 => OrderEvent::Released(OrderReleased { header, released_price: self.price(i, 0)? }),
            4 => OrderEvent::Submitted(OrderSubmitted { header, account_id }),
            5 => OrderEvent::Accepted(OrderAccepted {
                header,
                venue_order_id: self.venue_order_id(i, 1)?,
                account_id,
                reconciliation,
            }),
            6 => OrderEvent::Rejected(OrderRejected {
                header,
                account_id,
                reason: self.reason(i, 0)?,
                reconciliation,
                due_post_only: self.chance(1, 3, i, 420),
            }),
            7 => OrderEvent::Canceled(OrderCanceled {
                header,
                venue_order_id: opt_venue,
                account_id: opt_account,
                reason: if self.chance(1, 2, i, 421) { Some(self.reason(i, 0)?) } else { None },
                reconciliation,
            }),
            8 => OrderEvent::Expired(OrderExpired {
                header,
                venue_order_id: opt_venue,
                account_id: opt_account,
                reconciliation,
            }),
            9 => OrderEvent::Triggered(OrderTriggered {
                header,
                venue_order_id: opt_venue,
                account_id: opt_account,
                reconciliation,
            }),
            10 => OrderEvent::PendingUpdate(OrderPendingUpdate {
                header,
                account_id,
                venue_order_id: opt_venue,
                reconciliation,
            }),
            11 => OrderEvent::PendingCancel(OrderPendingCancel {
                header,
                account_id,
                venue_order_id: opt_venue,
                reconciliation,
            }),
            12 => OrderEvent::ModifyRejected(OrderModifyRejected {
                header,
                reason: self.reason(i, 0)?,
                venue_order_id: opt_venue,
                account_id: opt_account,
                reconciliation,
            }),
            13 => OrderEvent::CancelRejected(OrderCancelRejected {
                header,
                reason: self.reason(i, 0)?,
                venue_order_id: opt_venue,
                account_id: opt_account,
                reconciliation,
            }),
            14 => OrderEvent::Updated(OrderUpdated {
                header,
                venue_order_id: opt_venue,
                account_id: opt_account,
                quantity: self.size(i, 0)?,
                price: if self.chance(3, 4, i, 430) { Some(self.price(i, 0)?) } else { None },
                trigger_price: None,
                protection_price: None,
                is_quote_quantity: false,
                reconciliation,
            }),
            15 => OrderEvent::Filled(OrderFilled {
                header,
                venue_order_id: self.venue_order_id(i, 1)?,
                account_id,
                trade_id: self.trade_id(i, 2)?,
                order_side: self.pick(OrderSide::ALL, i, 440),
                order_type: self.pick(&[OrderType::Limit, OrderType::Market], i, 441),
                last_qty: self.size(i, 0)?,
                last_px: self.price(i, 0)?,
                currency: Self::usdt(),
                liquidity_side: self.pick(&[LiquiditySide::Maker, LiquiditySide::Taker], i, 442),
                reconciliation,
                position_id: if self.chance(1, 2, i, 443) {
                    Some(PositionId::netting(&header.instrument_id, &header.strategy_id)?)
                } else {
                    None
                },
                commission: if self.chance(3, 4, i, 444) {
                    Some(self.money(i, 0, Self::usdt())?)
                } else {
                    None
                },
                info_flags: FillInfoFlags::new(self.below(8, i, Draw::Flags, 0) as u8)?,
            }),
            _ => OrderEvent::FillVoided(OrderFillVoided {
                header,
                venue_order_id: self.venue_order_id(i, 1)?,
                account_id,
                correction_id: self.trade_id(i, 3)?,
                trade_id: self.trade_id(i, 2)?,
                voided_qty: self.size(i, 0)?,
                commission_voided: if self.chance(1, 2, i, 450) {
                    Some(self.money(i, 1, Self::usdt())?)
                } else {
                    None
                },
                order_side: self.pick(OrderSide::ALL, i, 451),
                order_type: OrderType::Limit,
                last_px: self.price(i, 0)?,
                currency: Self::usdt(),
                liquidity_side: LiquiditySide::Maker,
                position_id: None,
                reason: Some(self.reason(i, 1)?),
                reconciliation,
                is_reopened: self.chance(1, 4, i, 452),
                info_flags: FillInfoFlags::default(),
            }),
        })
    }
}

fn write_decimal(mut n: u64, out: &mut [u8; 20]) -> usize {
    if n == 0 {
        out[0] = b'0';
        return 1;
    }
    let mut digits = 0;
    let mut t = n;
    while t > 0 {
        digits += 1;
        t /= 10;
    }
    for i in (0..digits).rev() {
        out[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    digits
}
