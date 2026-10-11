//! The report of a backtest (docs/architecture.md section 12.4): fills, fees and PnL per strategy
//! and instrument, the orders' fate, and the account, labelled with the data and the fill model
//! the numbers come from. It is computed from the run directory alone (`config.toml` and the run
//! log): orders go through a fresh OMS and applied fills through a `Portfolio`, with the kernel's
//! own code, so the numbers are the kernel's.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;

use data::intern::InstrumentSlot;
use data::subscription::StrategyIndex;
use execution::fsm::OrderEventKind;
use execution::oms::{OrderIndex, OrderRecord};
use execution::{apply_order_event, EventOutcome, Oms};
use kernel_core::{Status, UnixNanos};
use model::decimal::format_raw;
use model::enums::{LiquiditySide, OrderSide, OrderStatus, OrderType, TimeInForce};
use model::instruments::Instrument;
use model::log::RecordBody;
use model::order_events::{OrderEvent, OrderFilled};
use model::outputs::Output;
use model::{ClientOrderId, Currency, Event, InstrumentId, Money, Price, Quantity};
use portfolio::{Portfolio, PortfolioConfig};

use crate::backtest_node::kernel_config;
use crate::config::{env_name, FillModel, NodeConfig, SelfTradePrevention};
use crate::event_log::{EventLogReadOptions, EventLogReader};
use crate::run_dir::load_run_config;
use crate::{NodeError, NodeResult};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReportRow {
    pub strategy: String,
    pub instrument: String,
    pub fills: u64,
    pub maker_fills: u64,
    pub taker_fills: u64,
    pub bought: String,
    pub sold: String,
    /// Traded notional in the settlement currency.
    pub notional: String,
    /// Paid (positive).
    pub commission: String,
    /// Received (positive) or paid.
    pub funding: String,
    /// Gross of commission and funding.
    pub realized: String,
    /// At the valuation price.
    pub unrealized: String,
    /// `realized - commission + funding + unrealized`.
    pub net: String,
    /// Signed, at the end.
    pub position: String,
    pub avg_px_open: Option<String>,
    pub valuation: Option<String>,
    /// `mark`, `last trade`, `last quote mid` or `none`.
    pub valuation_source: String,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OrderCounts {
    pub submitted: u64,
    pub denied: u64,
    pub accepted: u64,
    pub rejected: u64,
    pub canceled: u64,
    pub expired: u64,
    /// Orders that ended `FILLED`.
    pub filled: u64,
    pub modifies: u64,
    pub cancels: u64,
    pub modify_rejected: u64,
    pub cancel_rejected: u64,
    /// Venue events the OMS refused (duplicates, late events).
    pub refused: u64,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RunReport {
    pub directory: String,
    pub node_id: String,
    pub env: String,
    pub seed: u64,
    pub catalog: String,
    /// `start to end`, empty for the whole catalog.
    pub range: String,
    pub streams: Vec<String>,
    pub instruments: Vec<String>,
    /// The market data the fills were simulated against.
    pub book: String,
    /// The simulated venue's models, or `none (data only)`.
    pub venue: String,
    pub fill_model: String,
    pub fee_schedule: String,
    pub starting_balances: Vec<String>,
    pub ending_balances: Vec<String>,
    pub rows: Vec<ReportRow>,
    /// By strategy.
    pub orders: Vec<(String, OrderCounts)>,
    pub first_ts: u64,
    pub last_ts: u64,
}

fn millis(ns: u64) -> String {
    format!(
        "{}.{}{}{} ms",
        ns / 1_000_000,
        (ns / 100_000) % 10,
        (ns / 10_000) % 10,
        (ns / 1_000) % 10
    )
}

fn stp_name(s: SelfTradePrevention) -> &'static str {
    match s {
        SelfTradePrevention::None => "none",
        SelfTradePrevention::ExpireTaker => "expire_taker",
        SelfTradePrevention::ExpireMaker => "expire_maker",
        SelfTradePrevention::ExpireBoth => "expire_both",
    }
}

fn money(raw: i64, currency: Currency) -> Money {
    Money::from_raw(raw, currency).unwrap_or_else(|_| Money::zero(currency))
}

fn decimal(raw: i64, precision: u8) -> String {
    let mut buf = [0u8; 64];
    let n = format_raw(i128::from(raw), precision, &mut buf);
    String::from_utf8_lossy(&buf[..n]).into_owned()
}

fn labels(config: &NodeConfig, out: &mut RunReport) {
    out.node_id.clone_from(&config.node.id);
    out.env = env_name(config.node.env).to_string();
    out.seed = config.node.seed;
    out.catalog.clone_from(&config.data.catalog);
    if let Some(r) = &config.data.range {
        out.range = format!("{} to {}", r.start.to_rfc3339(), r.end.to_rfc3339());
    }
    let (mut depth, mut quotes, mut trades) = (false, false, false);
    for group in &config.data.streams {
        for stream in &group.streams {
            if !out.streams.contains(stream) {
                out.streams.push(stream.clone());
            }
            depth |= stream.starts_with("depth");
            quotes |= stream == "bookTicker";
            trades |= stream == "aggTrade" || stream == "trade";
        }
        for id in &group.instruments {
            let name = id.to_string();
            if !out.instruments.contains(&name) {
                out.instruments.push(name);
            }
        }
    }
    out.book = if depth {
        "L2 (depth)"
    } else if quotes {
        "L1 (bookTicker)"
    } else {
        "no book"
    }
    .to_string();
    if trades {
        out.book.push_str(" + trades (aggTrade)");
    }
    let Some(sim) = config.venues.first().and_then(|v| v.sim.as_ref()) else {
        out.venue = "none (data only)".to_string();
        return;
    };
    out.fill_model = match sim.fill_model {
        FillModel::QueuePosition => "queue_position",
        FillModel::TopOfBook => "top_of_book",
    }
    .to_string();
    out.fee_schedule = if sim.fee_schedule.is_empty() {
        "binance_usdm_vip0".to_string()
    } else {
        sim.fee_schedule.clone()
    };
    out.venue = format!(
        "simulated; fill model {}; latency feed {}, out {}, in {}, jitter {}; fees {}; STP {}",
        out.fill_model,
        millis(sim.latency.feed_ns),
        millis(sim.latency.out_ns),
        millis(sim.latency.in_ns),
        millis(sim.latency.jitter_ns),
        out.fee_schedule,
        stp_name(sim.stp)
    );
}

#[derive(Clone, Copy, Debug, Default)]
struct Acc {
    fills: u64,
    maker: u64,
    taker: u64,
    bought: i128,
    sold: i128,
    notional: i128,
}

#[derive(Clone, Debug)]
struct Market {
    id: InstrumentId,
    definition: Option<Instrument>,
    mid: Option<Price>,
    mark: bool,
    trade: bool,
}

/// Replays the orders of the log through a fresh OMS, as `strategy::Trading` saw them: the
/// kernel's commands create and move orders, the venue's events apply.
struct OrderReplay {
    oms: Oms,
}

#[derive(Clone, Copy, Debug)]
struct Outcome {
    index: OrderIndex,
    applied: bool,
    kind: OrderEventKind,
}

impl OrderReplay {
    fn new(orders: u32, fills: u32, slots: u32) -> Self {
        Self { oms: Oms::new(orders + 1, fills + 1, slots) }
    }

    #[allow(clippy::too_many_arguments)]
    fn create(
        &mut self,
        cid: ClientOrderId,
        strategy: u16,
        slot: u32,
        instrument_id: InstrumentId,
        side: OrderSide,
        order_type: OrderType,
        quantity: Quantity,
        price: Option<Price>,
        tif: TimeInForce,
        ts: UnixNanos,
        kind: OrderEventKind,
    ) -> Option<Outcome> {
        let record = OrderRecord::new(
            cid,
            strategy,
            slot,
            instrument_id,
            side,
            order_type,
            quantity,
            price,
            tif,
            ts,
        );
        let index = self.oms.create(record).ok()?;
        let applied = self.oms.apply(index, kind).is_ok();
        Some(Outcome { index, applied, kind })
    }

    fn internal(&mut self, cid: &ClientOrderId, kind: OrderEventKind) -> Option<Outcome> {
        let index = self.oms.find(cid)?;
        let applied = self.oms.apply(index, kind).is_ok();
        Some(Outcome { index, applied, kind })
    }

    fn on_output(
        &mut self,
        output: &Output,
        slot: impl FnOnce(&InstrumentId) -> u32,
    ) -> Option<Outcome> {
        match output {
            Output::SubmitOrder(s) => {
                let slot = slot(&s.instrument_id);
                self.create(
                    s.client_order_id,
                    s.strategy_index,
                    slot,
                    s.instrument_id,
                    s.order_side,
                    s.order_type,
                    s.quantity,
                    s.price,
                    s.time_in_force,
                    s.ts_init,
                    OrderEventKind::Submitted,
                )
            }
            Output::OrderDenied(d) => {
                let slot = slot(&d.header.instrument_id);
                // The denial does not carry the terms: quantity 1, no price.
                self.create(
                    d.header.client_order_id,
                    0,
                    slot,
                    d.header.instrument_id,
                    OrderSide::Buy,
                    OrderType::Limit,
                    Quantity::from_units(1, 0).unwrap_or_default(),
                    None,
                    TimeInForce::Gtc,
                    d.header.ts_init,
                    OrderEventKind::Denied,
                )
            }
            Output::ModifyOrder(m) => {
                self.internal(&m.client_order_id, OrderEventKind::PendingUpdate)
            }
            Output::CancelOrder(c) => {
                self.internal(&c.client_order_id, OrderEventKind::PendingCancel)
            }
            _ => None,
        }
    }

    fn on_event(&mut self, event: &OrderEvent) -> Option<Outcome> {
        let kind = OrderEventKind::of(event)?;
        let (outcome, index) = apply_order_event(&mut self.oms, event);
        let index = index?;
        Some(Outcome { index, applied: outcome == EventOutcome::Applied, kind })
    }
}

struct Reporter<'a> {
    config: &'a NodeConfig,
    portfolio: Portfolio,
    replay: OrderReplay,
    markets: Vec<Market>,
    accs: BTreeMap<(u16, u32), Acc>,
    orders: BTreeMap<String, OrderCounts>,
    start: Vec<Money>,
    have_start: bool,
    first_ts: u64,
    last_ts: u64,
}

impl<'a> Reporter<'a> {
    fn new(config: &'a NodeConfig, orders: u32, fills: u32) -> Self {
        let kernel = kernel_config(config);
        Self {
            config,
            portfolio: Portfolio::new(PortfolioConfig {
                instruments: config.node.capacity.instruments,
                strategies: kernel.strategies,
                margin: kernel.trading.margin,
                currencies: 16,
            }),
            replay: OrderReplay::new(orders, fills, config.node.capacity.instruments),
            markets: Vec::new(),
            accs: BTreeMap::new(),
            orders: BTreeMap::new(),
            start: Vec::new(),
            have_start: false,
            first_ts: 0,
            last_ts: 0,
        }
    }

    fn slot(&mut self, id: &InstrumentId) -> u32 {
        if let Some(i) = self.markets.iter().position(|m| m.id == *id) {
            return i as u32;
        }
        self.markets.push(Market {
            id: *id,
            definition: None,
            mid: None,
            mark: false,
            trade: false,
        });
        (self.markets.len() - 1) as u32
    }

    fn strategy_name(&self, s: u16) -> String {
        match self.config.strategies.get(s as usize) {
            Some(entry) if !entry.id.is_empty() => entry.id.clone(),
            _ => format!("strategy-{}", s + 1),
        }
    }

    fn counts(&mut self, strategy: &str) -> &mut OrderCounts {
        self.orders.entry(strategy.to_string()).or_default()
    }

    fn on_input(&mut self, ts: UnixNanos, event: &Event) {
        if self.first_ts == 0 {
            self.first_ts = ts.value();
        }
        self.last_ts = ts.value();
        match event {
            Event::Instrument(def) => {
                let s = self.slot(&def.id);
                self.markets[s as usize].definition = Some(*def);
            }
            Event::AccountState(a) => {
                if !self.have_start {
                    self.start.extend(a.balances.iter().map(|b| b.total));
                    self.have_start = true;
                }
                let _ = self.portfolio.set_account(a);
            }
            Event::TradeTick(t) => {
                let s = self.slot(&t.instrument_id);
                let _ = self.portfolio.on_trade_price(InstrumentSlot(s), t.price);
                self.markets[s as usize].trade = true;
            }
            Event::QuoteTick(q) => {
                let s = self.slot(&q.instrument_id);
                if let Ok(mid) = Price::from_raw(
                    (q.bid_price.raw() + q.ask_price.raw()) / 2,
                    model::FIXED_PRECISION,
                ) {
                    self.markets[s as usize].mid = Some(mid);
                }
            }
            Event::MarkPriceUpdate(m) => {
                let s = self.slot(&m.instrument_id);
                let _ = self.portfolio.on_mark(InstrumentSlot(s), m.value);
                self.markets[s as usize].mark = true;
            }
            Event::FundingRateUpdate(f) => {
                let s = self.slot(&f.instrument_id);
                if let Some(def) = self.markets[s as usize].definition {
                    let _ = self.portfolio.on_funding(&def, InstrumentSlot(s), f);
                }
            }
            Event::Order(e) => self.order_event(e),
            _ => {}
        }
    }

    fn on_output(&mut self, output: &Output) {
        let outcome = {
            let mut slots = Vec::new();
            let r = self.replay.on_output(output, |id| {
                slots.push(*id);
                0
            });
            // The slot is this report's market index; the OMS only needs it to be stable.
            if let (Some(o), Some(id)) = (r, slots.first()) {
                let slot = self.slot(id);
                if let Some(record) = self.replay.oms.get_mut(o.index) {
                    record.slot = slot;
                }
            }
            r
        };
        if let Output::OrderDenied(d) = output {
            self.counts(d.header.strategy_id.as_str()).denied += 1;
            return;
        }
        let Some(o) = outcome else { return };
        let strategy = self.replay.oms.get(o.index).map_or(0, |r| r.strategy);
        let name = self.strategy_name(strategy);
        let c = self.counts(&name);
        match output {
            Output::SubmitOrder(_) => c.submitted += 1,
            Output::ModifyOrder(_) => c.modifies += 1,
            Output::CancelOrder(_) => c.cancels += 1,
            _ => {}
        }
    }

    fn order_event(&mut self, event: &OrderEvent) {
        let Some(o) = self.replay.on_event(event) else { return };
        let Some(record) = self.replay.oms.get(o.index).copied() else { return };
        let name = self.strategy_name(record.strategy);
        if !o.applied {
            self.counts(&name).refused += 1;
            return;
        }
        let c = self.counts(&name);
        match o.kind {
            OrderEventKind::Accepted => c.accepted += 1,
            OrderEventKind::Rejected => c.rejected += 1,
            OrderEventKind::Canceled => c.canceled += 1,
            OrderEventKind::Expired => c.expired += 1,
            OrderEventKind::ModifyRejected => c.modify_rejected += 1,
            OrderEventKind::CancelRejected => c.cancel_rejected += 1,
            OrderEventKind::Filled => {
                if record.state.status() == OrderStatus::Filled {
                    c.filled += 1;
                }
                self.book(&record, event);
            }
            OrderEventKind::FillVoided => self.book(&record, event),
            _ => {}
        }
    }

    /// As `strategy::Trading` books: a fill, or a void as the opposite fill with its fee refunded.
    fn book(&mut self, o: &OrderRecord, event: &OrderEvent) {
        let slot = o.slot;
        let Some(def) = self.markets.get(slot as usize).and_then(|m| m.definition) else { return };
        let acc = self.accs.entry((o.strategy, slot)).or_default();
        let fill: OrderFilled = match event {
            OrderEvent::Filled(f) => {
                acc.fills += 1;
                acc.maker += u64::from(f.liquidity_side == LiquiditySide::Maker);
                acc.taker += u64::from(f.liquidity_side == LiquiditySide::Taker);
                let side =
                    if f.order_side == OrderSide::Buy { &mut acc.bought } else { &mut acc.sold };
                *side += i128::from(f.last_qty.raw());
                if let Ok(n) =
                    model::fixed_point::notional_raw(f.last_px, f.last_qty, def.multiplier)
                {
                    acc.notional += i128::from(n);
                }
                *f
            }
            OrderEvent::FillVoided(v) => {
                let side = if o.side == OrderSide::Buy { &mut acc.bought } else { &mut acc.sold };
                *side -= i128::from(v.voided_qty.raw());
                OrderFilled {
                    header: v.header,
                    venue_order_id: v.venue_order_id,
                    account_id: v.account_id,
                    trade_id: v.trade_id,
                    order_side: if o.side == OrderSide::Buy {
                        OrderSide::Sell
                    } else {
                        OrderSide::Buy
                    },
                    order_type: v.order_type,
                    last_qty: v.voided_qty,
                    last_px: v.last_px,
                    currency: v.currency,
                    liquidity_side: v.liquidity_side,
                    reconciliation: v.reconciliation,
                    position_id: v.position_id,
                    commission: v.commission_voided.map(|c| money(-c.raw(), c.currency())),
                    info_flags: model::order_events::FillInfoFlags::default(),
                }
            }
            _ => return,
        };
        let _ =
            self.portfolio.on_fill(&def, InstrumentSlot(slot), StrategyIndex(o.strategy), &fill);
    }

    fn row(&mut self, strategy: u16, slot: u32, acc: &Acc) -> ReportRow {
        let m = &self.markets[slot as usize];
        let mut r = ReportRow {
            strategy: self.strategy_name(strategy),
            instrument: m.id.to_string(),
            fills: acc.fills,
            maker_fills: acc.maker,
            taker_fills: acc.taker,
            ..ReportRow::default()
        };
        let Some(def) = m.definition else { return r };
        let ccy = def.settlement_currency();
        let size = def.size_precision;
        r.bought = decimal(acc.bought as i64, size);
        r.sold = decimal(acc.sold as i64, size);
        r.notional = money(acc.notional as i64, ccy).to_string();
        r.valuation_source = "none".to_string();
        if m.mark {
            r.valuation_source = "mark".to_string();
        } else if m.trade {
            r.valuation_source = "last trade".to_string();
        } else if let Some(mid) = m.mid {
            let _ = self.portfolio.on_trade_price(InstrumentSlot(slot), mid);
            r.valuation_source = "last quote mid".to_string();
        }
        r.valuation = self.portfolio.valuation(InstrumentSlot(slot)).map(|p| p.to_string());
        let Some(p) =
            self.portfolio.position(StrategyIndex(strategy), InstrumentSlot(slot)).copied()
        else {
            return r;
        };
        let unrealized =
            self.portfolio.unrealized(&def, InstrumentSlot(slot), &p).map_or(0, |m| m.raw());
        r.realized = money(p.total_realized_raw(), ccy).to_string();
        r.commission = money(p.total_commission_raw(), ccy).to_string();
        r.funding = money(p.total_funding_raw(), ccy).to_string();
        r.unrealized = money(unrealized, ccy).to_string();
        r.net = money(
            p.total_realized_raw() - p.total_commission_raw() + p.total_funding_raw() + unrealized,
            ccy,
        )
        .to_string();
        r.position = decimal(p.signed_raw(), size);
        if p.is_open() {
            r.avg_px_open = p.avg_px_open().map(|px| px.to_string());
        }
        r
    }

    fn finish(mut self, out: &mut RunReport) {
        out.starting_balances = self.start.iter().map(ToString::to_string).collect();
        for m in &self.start {
            if let Some(w) = self.portfolio.wallet(m.currency()) {
                out.ending_balances.push(w.to_string());
            }
        }
        let accs: Vec<((u16, u32), Acc)> = self.accs.iter().map(|(k, v)| (*k, *v)).collect();
        for ((strategy, slot), acc) in accs {
            out.rows.push(self.row(strategy, slot, &acc));
        }
        out.orders = self.orders.into_iter().collect();
        out.first_ts = self.first_ts;
        out.last_ts = self.last_ts;
    }
}

/// Upper bounds of the orders and fills in the log.
fn count(directory: &Path) -> kernel_core::Result<(u32, u32)> {
    let reader = EventLogReader::open(directory, EventLogReadOptions::default())?;
    let (mut orders, mut fills) = (0u32, 0u32);
    for record in reader {
        match record?.body {
            RecordBody::Output(Output::SubmitOrder(_) | Output::OrderDenied(_)) => orders += 1,
            RecordBody::Input(Event::Order(OrderEvent::Filled(_))) => fills += 1,
            _ => {}
        }
    }
    Ok((orders, fills))
}

pub fn build_run_report(directory: &Path) -> NodeResult<RunReport> {
    let shown = directory.display().to_string();
    let mut out = RunReport { directory: shown.clone(), ..RunReport::default() };
    let config = load_run_config(directory)?;
    labels(&config, &mut out);
    let (orders, fills) = count(directory)
        .map_err(|s| NodeError::new(s, format!("cannot read the run log in {shown}")))?;
    let mut reporter = Reporter::new(&config, orders, fills);
    let reader = EventLogReader::open(directory, EventLogReadOptions::default())
        .map_err(|s| NodeError::new(s, format!("cannot read the run log in {shown}")))?;
    for record in reader {
        let record = record
            .map_err(|s| NodeError::new(s, format!("cannot decode the run log in {shown}")))?;
        match &record.body {
            RecordBody::Output(o) => reporter.on_output(o),
            RecordBody::Input(e) => reporter.on_input(record.key.ts, e),
        }
    }
    reporter.finish(&mut out);
    Ok(out)
}

/// The report as text, one fact per line.
#[must_use]
pub fn report_text(r: &RunReport) -> String {
    let join = |items: &[String]| items.join(", ");
    let balances =
        |items: &[String]| if items.is_empty() { "none".to_string() } else { items.join(", ") };
    let mut out = String::new();
    let _ = writeln!(out, "run: {}", r.directory);
    let _ = writeln!(out, "node: {} ({}, seed {})", r.node_id, r.env, r.seed);
    let _ = writeln!(
        out,
        "data: {} for {} from {}{}",
        join(&r.streams),
        join(&r.instruments),
        r.catalog,
        if r.range.is_empty() { String::new() } else { format!(", {}", r.range) }
    );
    let _ = writeln!(out, "fills simulated against: {}", r.book);
    let _ = writeln!(out, "venue: {}", r.venue);
    let _ = writeln!(
        out,
        "account: {} -> {}",
        balances(&r.starting_balances),
        balances(&r.ending_balances)
    );
    for row in &r.rows {
        let _ = writeln!(out, "strategy {} on {}:", row.strategy, row.instrument);
        let _ = writeln!(
            out,
            "  fills {} (maker {}, taker {}), bought {}, sold {}, notional {}",
            row.fills, row.maker_fills, row.taker_fills, row.bought, row.sold, row.notional
        );
        let _ = writeln!(
            out,
            "  realized {}, commission {}, funding {}",
            row.realized, row.commission, row.funding
        );
        let _ = write!(out, "  position {}", row.position);
        if let Some(avg) = &row.avg_px_open {
            let _ = write!(out, " at {avg}");
        }
        let _ = write!(out, ", unrealized {}", row.unrealized);
        if let Some(v) = &row.valuation {
            let _ = write!(out, " at {v} ({})", row.valuation_source);
        }
        let _ = writeln!(out, "\n  net {}", row.net);
    }
    for (strategy, c) in &r.orders {
        let _ = writeln!(
            out,
            "orders of {strategy}: submitted {}, denied {}, accepted {}, rejected {}, filled {}, \
             canceled {}, expired {}; modifies {} ({} rejected), cancels {} ({} rejected); \
             refused events {}",
            c.submitted,
            c.denied,
            c.accepted,
            c.rejected,
            c.filled,
            c.canceled,
            c.expired,
            c.modifies,
            c.modify_rejected,
            c.cancels,
            c.cancel_rejected,
            c.refused
        );
    }
    out
}

#[allow(dead_code)]
fn _status_is_used(_: Status) {}
