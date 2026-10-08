//! The portfolio, updated inside `step` from the fills the OMS accepted, mark prices, funding and
//! account snapshots:
//!
//! - venue positions: one netting position per instrument, what the account holds, the basis of
//!   balances, margin and exposure;
//! - the ledger: one netting position per (strategy, instrument), each strategy's share from its
//!   own fills; the strategy's position events and PnL come from here. The venue position always
//!   equals the sum of the strategies' shares;
//! - balances: wallet balance per currency, set by `AccountState`, then moved by realized PnL,
//!   commissions and funding of the venue positions.
//!
//! Linear derivatives only (USDⓈ-M perpetuals and futures). Fills of inverse contracts and spot
//! pairs are counted and not booked; they arrive with the venues that trade them.

use cost::{funding as funding_of, Rate};
use data::intern::InstrumentSlot;
use data::subscription::StrategyIndex;
use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{FixedVec, Result, Status, UnixNanos};
use model::account::AccountState;
use model::data::FundingRateUpdate;
use model::instruments::{Instrument, InstrumentKind};
use model::order_events::OrderFilled;
use model::{Currency, Money, Price, Quantity};

use crate::margin::MarginModel;
use crate::position::NettingPosition;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PortfolioConfig {
    pub instruments: u32,
    pub strategies: u16,
    pub margin: MarginModel,
    /// Distinct currencies the account may hold.
    pub currencies: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PortfolioStats {
    pub fills: u64,
    pub fills_not_booked: u64,
    pub funding_settlements: u64,
    pub commissions_booked: u64,
}
kernel_core::state_fields!(PortfolioStats {
    fills,
    fills_not_booked,
    funding_settlements,
    commissions_booked
});

/// What one part of a fill did to the strategy's position.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PositionStep {
    Opened,
    Changed,
    Closed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FillPart {
    pub step: PositionStep,
    pub quantity: Quantity,
    pub realized: Money,
}

/// Result of a fill for the strategy's position: one part, or two when the fill flipped it
/// (closed, then opened on the other side). `None` when the fill was counted and not booked.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FillOutcome {
    pub parts: [Option<FillPart>; 2],
}

impl FillOutcome {
    pub fn iter(&self) -> impl Iterator<Item = &FillPart> {
        self.parts.iter().flatten()
    }
}

/// A funding settlement: the venue position's flow (moves the balance) and each strategy's share.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FundingSettlement {
    pub rate: Rate,
    pub mark: Price,
    pub venue: Money,
    pub shares: FixedVec<(StrategyIndex, Money)>,
}

#[derive(Clone, Debug)]
pub struct Portfolio {
    config: PortfolioConfig,
    venue: FixedVec<NettingPosition>,
    ledger: FixedVec<NettingPosition>,
    balances: FixedVec<(Currency, i64)>,
    marks: FixedVec<Option<Price>>,
    last_trades: FixedVec<Option<Price>>,
    /// Per instrument: the stored funding rate and the time it settles.
    pending_funding: FixedVec<Option<(Rate, UnixNanos)>>,
    stats: PortfolioStats,
}

impl Portfolio {
    #[must_use]
    pub fn new(config: PortfolioConfig) -> Self {
        let n = config.instruments as usize;
        Self {
            config,
            venue: filled(n),
            ledger: filled(n * config.strategies as usize),
            balances: FixedVec::with_capacity(config.currencies as usize),
            marks: filled(n),
            last_trades: filled(n),
            pending_funding: filled(n),
            stats: PortfolioStats::default(),
        }
    }

    #[must_use]
    pub const fn config(&self) -> &PortfolioConfig {
        &self.config
    }

    // ---- inputs -------------------------------------------------------------------------------

    /// An account snapshot replaces every balance.
    pub fn set_account(&mut self, state: &AccountState) -> Result<()> {
        self.balances.clear();
        for b in &state.balances {
            self.balances.push((b.currency, b.total.raw()))?;
        }
        Ok(())
    }

    /// Reconciliation sets the venue position of `slot`; the strategies' shares stay as their
    /// fills made them.
    pub fn set_venue_position(
        &mut self,
        slot: InstrumentSlot,
        signed_raw: i64,
        avg_px: Price,
        ts: UnixNanos,
    ) -> Result<()> {
        self.venue.get_mut(slot.index()).ok_or(Status::OutOfRange)?.set(signed_raw, avg_px, ts)
    }

    pub fn on_mark(&mut self, slot: InstrumentSlot, mark: Price) -> Result<()> {
        *self.marks.get_mut(slot.index()).ok_or(Status::OutOfRange)? = Some(mark);
        Ok(())
    }
    pub fn on_trade_price(&mut self, slot: InstrumentSlot, price: Price) -> Result<()> {
        *self.last_trades.get_mut(slot.index()).ok_or(Status::OutOfRange)? = Some(price);
        Ok(())
    }

    /// Books a fill of `strategy`. The commission is booked as reported (positive paid).
    pub fn on_fill(
        &mut self,
        instrument: &Instrument,
        slot: InstrumentSlot,
        strategy: StrategyIndex,
        fill: &OrderFilled,
    ) -> Result<Option<FillOutcome>> {
        self.stats.fills += 1;
        if !Self::is_linear(instrument) {
            self.stats.fills_not_booked += 1;
            return Ok(None);
        }
        let multiplier = instrument.multiplier.raw();
        let settlement = instrument.settlement_currency();
        let index = self.ledger_index(strategy, slot)?;
        let ts = fill.header.ts_init;
        let order_id = fill.header.client_order_id;

        let mut outcome = FillOutcome { parts: [None, None] };
        let mut remaining = fill.last_qty;
        let mut part = 0;
        while !remaining.is_zero() {
            let closable = self.ledger[index].closable(fill.order_side);
            let qty_raw =
                if closable == 0 { remaining.raw() } else { remaining.raw().min(closable) };
            let qty = Quantity::from_raw(qty_raw, remaining.precision())?;
            let applied = self.ledger[index].apply(
                fill.order_side,
                qty,
                fill.last_px,
                multiplier,
                order_id,
                ts,
            )?;
            let step = if applied.opened {
                PositionStep::Opened
            } else if applied.closed {
                PositionStep::Closed
            } else {
                PositionStep::Changed
            };
            outcome.parts[part] = Some(FillPart {
                step,
                quantity: qty,
                realized: Money::from_raw(applied.realized_raw, settlement)?,
            });
            part += 1;
            remaining = remaining.checked_sub(qty)?;
        }
        // The venue position sees the same fill, split where it flips.
        let mut remaining = fill.last_qty;
        while !remaining.is_zero() {
            let venue = &mut self.venue[slot.index()];
            let closable = venue.closable(fill.order_side);
            let qty_raw =
                if closable == 0 { remaining.raw() } else { remaining.raw().min(closable) };
            let qty = Quantity::from_raw(qty_raw, remaining.precision())?;
            let applied =
                venue.apply(fill.order_side, qty, fill.last_px, multiplier, order_id, ts)?;
            self.credit(settlement, applied.realized_raw)?;
            remaining = remaining.checked_sub(qty)?;
        }
        if let Some(commission) = fill.commission {
            self.book_commission(slot, index, commission)?;
        }
        Ok(Some(outcome))
    }

    /// A commission reported after its fill (Binance sends `TRADE_LITE` without it first).
    pub fn on_commission(
        &mut self,
        slot: InstrumentSlot,
        strategy: StrategyIndex,
        commission: Money,
    ) -> Result<()> {
        let index = self.ledger_index(strategy, slot)?;
        self.book_commission(slot, index, commission)
    }

    fn book_commission(
        &mut self,
        slot: InstrumentSlot,
        ledger_index: usize,
        commission: Money,
    ) -> Result<()> {
        self.stats.commissions_booked += 1;
        self.credit(commission.currency(), -commission.raw())?;
        self.venue[slot.index()].add_commission(commission.raw())?;
        self.ledger[ledger_index].add_commission(commission.raw())
    }

    /// A funding rate update. With `next_funding_ns`, a settlement happens when the next funding
    /// time moves past the stored one: the stored rate is paid at the valuation price. Without it,
    /// the update is itself a settled rate (historical funding files).
    pub fn on_funding(
        &mut self,
        instrument: &Instrument,
        slot: InstrumentSlot,
        update: &FundingRateUpdate,
    ) -> Result<Option<FundingSettlement>> {
        let rate = Rate(update.rate.raw());
        let pending = self.pending_funding.get_mut(slot.index()).ok_or(Status::OutOfRange)?;
        let settle_rate = match update.next_funding_ns {
            None => Some(rate),
            Some(next) => {
                let due = pending.take_if(|(_, stored_next)| next > *stored_next).map(|(r, _)| r);
                *pending = Some((rate, next));
                due
            }
        };
        let Some(settle_rate) = settle_rate else { return Ok(None) };
        self.settle_funding(instrument, slot, settle_rate).map(Some)
    }

    fn settle_funding(
        &mut self,
        instrument: &Instrument,
        slot: InstrumentSlot,
        rate: Rate,
    ) -> Result<FundingSettlement> {
        let mark = self.valuation(slot).ok_or(Status::InvalidState)?;
        let venue_flow = funding_of(instrument, self.venue[slot.index()].signed_raw(), mark, rate)?;
        self.venue[slot.index()].add_funding(venue_flow.raw())?;
        self.credit(venue_flow.currency(), venue_flow.raw())?;
        let mut shares = FixedVec::with_capacity(self.config.strategies as usize);
        for s in 0..self.config.strategies {
            let strategy = StrategyIndex(s);
            let index = self.ledger_index(strategy, slot)?;
            if !self.ledger[index].is_open() {
                continue;
            }
            let share = funding_of(instrument, self.ledger[index].signed_raw(), mark, rate)?;
            self.ledger[index].add_funding(share.raw())?;
            shares.push((strategy, share))?;
        }
        self.stats.funding_settlements += 1;
        Ok(FundingSettlement { rate, mark, venue: venue_flow, shares })
    }

    // ---- queries ------------------------------------------------------------------------------

    #[must_use]
    pub fn venue(&self, slot: InstrumentSlot) -> Option<&NettingPosition> {
        self.venue.get(slot.index())
    }
    #[must_use]
    pub fn position(
        &self,
        strategy: StrategyIndex,
        slot: InstrumentSlot,
    ) -> Option<&NettingPosition> {
        self.ledger_index(strategy, slot).ok().map(|i| &self.ledger[i])
    }

    /// The price positions are valued at: the mark price, else the last trade, else none.
    #[must_use]
    pub fn valuation(&self, slot: InstrumentSlot) -> Option<Price> {
        self.marks
            .get(slot.index())
            .copied()
            .flatten()
            .or_else(|| self.last_trades.get(slot.index()).copied().flatten())
    }

    /// Unrealized PnL of a position at the valuation price; zero without a price.
    pub fn unrealized(
        &self,
        instrument: &Instrument,
        slot: InstrumentSlot,
        position: &NettingPosition,
    ) -> Result<Money> {
        let settlement = instrument.settlement_currency();
        match self.valuation(slot) {
            None => Ok(Money::zero(settlement)),
            Some(mark) => {
                Money::from_raw(position.unrealized(mark, instrument.multiplier.raw())?, settlement)
            }
        }
    }

    /// Initial and maintenance margin of the venue position at the valuation price (else at the
    /// average open price).
    pub fn margins(&self, instrument: &Instrument, slot: InstrumentSlot) -> Result<(Money, Money)> {
        let position = self.venue(slot).ok_or(Status::OutOfRange)?;
        let settlement = instrument.settlement_currency();
        if !position.is_open() {
            return Ok((Money::zero(settlement), Money::zero(settlement)));
        }
        let price =
            self.valuation(slot).or_else(|| position.avg_px_open()).ok_or(Status::InvalidState)?;
        let notional =
            model::fixed_point::notional_raw(price, position.quantity()?, instrument.multiplier)?
                .unsigned_abs();
        Ok((
            self.config.margin.initial(instrument, notional)?,
            self.config.margin.maintenance(instrument, notional)?,
        ))
    }

    /// Initial margin an order of `quantity` at `price` would need.
    pub fn order_margin(
        &self,
        instrument: &Instrument,
        price: Price,
        quantity: Quantity,
    ) -> Result<Money> {
        let notional = model::fixed_point::notional_raw(price, quantity, instrument.multiplier)?
            .unsigned_abs();
        self.config.margin.initial(instrument, notional)
    }

    /// Wallet balance of `currency`; `None` when the account never held it.
    #[must_use]
    pub fn wallet(&self, currency: Currency) -> Option<Money> {
        self.balances
            .iter()
            .find(|(c, _)| *c == currency)
            .and_then(|&(c, raw)| Money::from_raw(raw, c).ok())
    }
    /// Currencies with a balance, in the order they first appeared.
    pub fn currencies(&self) -> impl Iterator<Item = Currency> + '_ {
        self.balances.iter().map(|(c, _)| *c)
    }
    #[must_use]
    pub const fn stats(&self) -> &PortfolioStats {
        &self.stats
    }

    /// Whether the venue position equals the sum of the strategies' shares, for every instrument.
    #[must_use]
    pub fn ledger_is_consistent(&self) -> bool {
        (0..self.config.instruments).all(|slot| {
            let sum: i64 = (0..self.config.strategies)
                .filter_map(|s| self.ledger_index(StrategyIndex(s), InstrumentSlot(slot)).ok())
                .map(|i| self.ledger[i].signed_raw())
                .sum();
            sum == self.venue[slot as usize].signed_raw()
        })
    }

    fn is_linear(instrument: &Instrument) -> bool {
        matches!(
            instrument.kind,
            InstrumentKind::CryptoPerpetual { is_inverse: false, .. }
                | InstrumentKind::CryptoFuture { is_inverse: false, .. }
        )
    }

    fn ledger_index(&self, strategy: StrategyIndex, slot: InstrumentSlot) -> Result<usize> {
        if strategy.0 >= self.config.strategies || slot.0 >= self.config.instruments {
            return Err(Status::OutOfRange);
        }
        Ok(strategy.0 as usize * self.config.instruments as usize + slot.index())
    }

    fn credit(&mut self, currency: Currency, raw: i64) -> Result<()> {
        if raw == 0 {
            return Ok(());
        }
        if let Some(entry) = self.balances.iter_mut().find(|(c, _)| *c == currency) {
            entry.1 = entry.1.checked_add(raw).ok_or(Status::Overflow)?;
            return Ok(());
        }
        self.balances.push((currency, raw))
    }
}

/// The shape comes from the configuration; the snapshot holds positions, balances, prices,
/// pending funding and the counters.
impl State for Portfolio {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.venue.write(w);
        self.ledger.write(w);
        w.u32(self.balances.len() as u32);
        for (currency, raw) in &self.balances {
            currency.write(w);
            raw.write(w);
        }
        self.marks.write(w);
        self.last_trades.write(w);
        w.u32(self.pending_funding.len() as u32);
        for pending in &self.pending_funding {
            match pending {
                None => w.u8(0),
                Some((rate, next)) => {
                    w.u8(1);
                    rate.0.write(w);
                    next.write(w);
                }
            }
        }
        self.stats.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.venue.read(r);
        self.ledger.read(r);
        let n = kernel_core::state::read_length(r, self.balances.capacity());
        self.balances.clear();
        for _ in 0..n {
            let mut currency = Currency::default();
            currency.read(r);
            let mut raw = 0i64;
            raw.read(r);
            let _ = self.balances.push((currency, raw));
        }
        self.marks.read(r);
        self.last_trades.read(r);
        if r.u32() as usize != self.pending_funding.len() {
            r.fail(Status::CapacityExceeded);
            return;
        }
        for pending in &mut self.pending_funding {
            *pending = match r.u8() {
                0 => None,
                1 => {
                    let mut rate = 0i64;
                    rate.read(r);
                    let mut next = UnixNanos::default();
                    next.read(r);
                    Some((Rate(rate), next))
                }
                _ => {
                    r.fail(Status::InvalidArgument);
                    return;
                }
            };
        }
        self.stats.read(r);
    }
}

/// A vector of `len` default values.
fn filled<T: Default>(len: usize) -> FixedVec<T> {
    let mut v = FixedVec::with_capacity(len);
    for _ in 0..len {
        let _ = v.push(T::default());
    }
    v
}
