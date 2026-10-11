//! `rust:Probe`: the strategy of the golden replay cases (the C++ tree's `GoldenStrategy`). Its
//! `plan` parameter picks what it exercises:
//!
//! - `trade`: every trade; `batch`: trades per batch;
//! - `quote`: conflated quotes; `book`: L2 deltas and the book;
//! - `bar`: external klines, internal time and tick bars;
//! - `feature`: EMA, VWAP, imbalance, microprice, realized volatility;
//! - `orders`: quotes both sides against the simulated venue and exercises every order command.
//!
//! Every observation is a `StrategyRecord` output, so a replay checks it.

use data::book::BookView;
use data::{FeatureId, FeatureKind, FeatureValue};
use kernel_core::state::{StateReader, StateWriter};
use kernel_core::{Result, Status, UnixNanos};
use model::bar::{Bar, BarType};
use model::data::{OrderBookDeltas, QuoteTick, TradeTick};
use model::enums::{BarAggregation, OrderSide, OrderStatus, TimeInForce};
use model::{ClientOrderId, InstrumentId, Price, Quantity};
use strategy::{Cadence, Context, OrderIntent, Strategy};

use crate::config::StrategyConfig;
use crate::{NodeError, NodeResult};

const INSTRUMENT: &str = "BTCUSDT-PERP.BINANCE";
const SCALE: i64 = 1_000_000_000;

#[derive(Clone, Debug)]
pub struct Probe {
    plan: String,
    instrument: InstrumentId,
    quotes: u64,
    bid: Option<ClientOrderId>,
    ask: Option<ClientOrderId>,
}

fn qty(thousandths: u64) -> Quantity {
    Quantity::from_raw(thousandths * 1_000_000, 3).unwrap_or_default()
}

fn is_open(s: OrderStatus) -> bool {
    matches!(
        s,
        OrderStatus::Submitted
            | OrderStatus::Accepted
            | OrderStatus::Triggered
            | OrderStatus::PendingUpdate
            | OrderStatus::PendingCancel
            | OrderStatus::PartiallyFilled
    )
}

/// A modify the order's state does not allow now (a fill got there first) is not an error.
fn ignore_busy(r: Result<()>) -> Result<()> {
    match r {
        Err(Status::InvalidState) => Ok(()),
        other => other,
    }
}

impl Probe {
    /// The factory for the registry: `params.plan` names the plan, `params.instrument` the
    /// instrument (default `BTCUSDT-PERP.BINANCE`).
    pub fn create(entry: &StrategyConfig) -> NodeResult<Box<dyn Strategy>> {
        let plan = entry.param_str("plan").ok_or_else(|| {
            NodeError::new(Status::InvalidArgument, "strategies: rust:Probe needs params.plan")
        })?;
        let instrument = entry.param_str("instrument").unwrap_or(INSTRUMENT);
        let instrument = InstrumentId::parse(instrument).map_err(|_| {
            NodeError::new(
                Status::InvalidArgument,
                format!("strategies: params.instrument \"{instrument}\" is not an instrument id"),
            )
        })?;
        Ok(Box::new(Self::new(plan, instrument)))
    }

    #[must_use]
    pub fn new(plan: &str, instrument: InstrumentId) -> Self {
        Self { plan: plan.to_string(), instrument, quotes: 0, bid: None, ask: None }
    }

    fn bar_types(&self) -> Result<[BarType; 4]> {
        let id = self.instrument;
        Ok([
            BarType::parse(&format!("{id}-1-MINUTE-LAST-EXTERNAL"))?,
            BarType::parse(&format!("{id}-1-MINUTE-LAST-INTERNAL"))?,
            BarType::parse(&format!("{id}-10-TICK-LAST-INTERNAL"))?,
            BarType::parse(&format!("{id}-30-SECOND-MID-INTERNAL"))?,
        ])
    }

    /// One resting order per side at the touch, moved with modifies as the touch moves. On top of
    /// that, on a fixed rhythm of quotes: cancel everything (7), a post-only buy at the ask that
    /// the venue rejects (11), a market sell (13), an IOC buy below the bid that expires (17), an
    /// order whose quantity has the wrong precision, which the risk checks deny (19), and a
    /// modify that raises the bid's quantity (23). Latency makes commands race with fills.
    fn orders(&mut self, ctx: &mut Context<'_>, q: &QuoteTick) -> Result<()> {
        self.quotes += 1;
        let id = self.instrument;
        let n = self.quotes;
        if n % 7 == 0 {
            self.bid = None;
            self.ask = None;
            return ctx.cancel_all(Some(&id)).map(|_| ());
        }
        Self::keep(ctx, &mut self.bid, id, OrderSide::Buy, q.bid_price)?;
        Self::keep(ctx, &mut self.ask, id, OrderSide::Sell, q.ask_price)?;
        if n % 11 == 0 {
            ctx.submit(
                &OrderIntent::limit(id, OrderSide::Buy, qty(10), q.ask_price, TimeInForce::Gtc)
                    .post_only(true),
            )?;
        }
        if n % 13 == 0 {
            ctx.submit(&OrderIntent::market(id, OrderSide::Sell, qty(5)))?;
        }
        if n % 17 == 0 {
            let below = Price::from_raw(q.bid_price.raw() - 100_000_000, 1)?;
            ctx.submit(&OrderIntent::limit(id, OrderSide::Buy, qty(10), below, TimeInForce::Ioc))?;
        }
        if n % 19 == 0 {
            let odd = Quantity::from_raw(500_000, 4)?; // 0.0005: finer than the step
            ctx.submit(&OrderIntent::limit(
                id,
                OrderSide::Buy,
                odd,
                q.bid_price,
                TimeInForce::Gtc,
            ))?;
        }
        if n % 23 == 0 {
            if let Some(bid) = self.bid {
                ignore_busy(ctx.modify(&bid, Some(qty(20)), None))?;
            }
        }
        Ok(())
    }

    /// Keeps one open order on `side` at `price`: modifies the working order, or submits a new one.
    fn keep(
        ctx: &mut Context<'_>,
        slot: &mut Option<ClientOrderId>,
        id: InstrumentId,
        side: OrderSide,
        price: Price,
    ) -> Result<()> {
        if let Some(view) = slot.as_ref().and_then(|cid| ctx.order(cid)) {
            if is_open(view.status) {
                if view.price == Some(price)
                    || matches!(
                        view.status,
                        OrderStatus::PendingUpdate | OrderStatus::PendingCancel
                    )
                {
                    return Ok(());
                }
                return ignore_busy(ctx.modify(&view.client_order_id, None, Some(price)));
            }
        }
        let cid = ctx.submit(&OrderIntent::limit(id, side, qty(10), price, TimeInForce::Gtc))?;
        *slot = Some(cid);
        Ok(())
    }
}

impl Strategy for Probe {
    fn on_start(&mut self, ctx: &mut Context<'_>) -> Result<()> {
        let id = self.instrument;
        match self.plan.as_str() {
            "trade" => ctx.subscribe_trades(&id, Cadence::Every),
            "batch" => ctx.subscribe_trades(&id, Cadence::OnBatch),
            "quote" => ctx.subscribe_quotes(&id, Cadence::Conflated),
            "book" => ctx.subscribe_book(&id, Cadence::sampled_ms(10_000)?),
            "bar" => {
                for t in self.bar_types()? {
                    ctx.subscribe_bars(&t, Cadence::Every)?;
                }
                Ok(())
            }
            "feature" => {
                ctx.feature(FeatureKind::Ema { period: 10 }, &id, Cadence::Every)?;
                ctx.feature(FeatureKind::Vwap { window: 20 }, &id, Cadence::sampled_ms(30_000)?)?;
                ctx.feature(FeatureKind::Imbalance, &id, Cadence::Conflated)?;
                ctx.feature(FeatureKind::Microprice, &id, Cadence::sampled_ms(60_000)?)?;
                ctx.feature(
                    FeatureKind::RealizedVol { window: 20 },
                    &id,
                    Cadence::sampled_ms(60_000)?,
                )?;
                Ok(())
            }
            "orders" => ctx.subscribe_quotes(&id, Cadence::Every),
            _ => Err(Status::InvalidArgument),
        }
    }

    fn on_trade(&mut self, ctx: &mut Context<'_>, t: &TradeTick) -> Result<()> {
        ctx.record("trade", t.price.raw())
    }

    fn on_trade_batch(
        &mut self,
        ctx: &mut Context<'_>,
        _: &InstrumentId,
        trades: &[TradeTick],
    ) -> Result<()> {
        ctx.record("batch", trades.len() as i64 * SCALE)
    }

    fn on_quote(&mut self, ctx: &mut Context<'_>, q: &QuoteTick) -> Result<()> {
        if self.plan == "orders" {
            return self.orders(ctx, q);
        }
        ctx.record("mid", (q.bid_price.raw() + q.ask_price.raw()) / 2)
    }

    fn on_book_deltas(&mut self, ctx: &mut Context<'_>, deltas: &OrderBookDeltas) -> Result<()> {
        ctx.record("deltas", deltas.deltas.len() as i64 * SCALE)
    }

    fn on_book(
        &mut self,
        ctx: &mut Context<'_>,
        _: &InstrumentId,
        book: &BookView<'_>,
    ) -> Result<()> {
        let (Some(bid), Some(ask)) = (book.best_bid(), book.best_ask()) else { return Ok(()) };
        ctx.record("bid", bid.price.raw())?;
        ctx.record("ask", ask.price.raw())
    }

    fn on_bar(&mut self, ctx: &mut Context<'_>, bar: &Bar) -> Result<()> {
        let tag =
            if bar.bar_type.spec.aggregation == BarAggregation::Tick { "tick_bar" } else { "bar" };
        ctx.record(tag, bar.close.raw())
    }

    fn on_feature(
        &mut self,
        ctx: &mut Context<'_>,
        id: FeatureId,
        value: FeatureValue,
        _: UnixNanos,
        _: UnixNanos,
    ) -> Result<()> {
        ctx.record(&format!("feature{}", id.0), value.0)
    }

    fn has_state(&self) -> bool {
        true
    }
    fn save_state(&self, w: &mut StateWriter<'_>) {
        use kernel_core::state::State;
        self.quotes.write(w);
        self.bid.write(w);
        self.ask.write(w);
    }
    fn load_state(&mut self, r: &mut StateReader<'_>) {
        use kernel_core::state::State;
        self.quotes.read(r);
        self.bid.read(r);
        self.ask.read(r);
    }
}
