//! What a strategy may do during a callback (docs/architecture.md section 9.4): read the time and
//! its random stream, subscribe, place orders, query copies of kernel state, record values.

use data::book::BookView;
use data::intern::InstrumentSlot;
use data::subscription::{Cadence, DataKind, StrategyIndex};
use data::{FeatureId, FeatureKind};
use execution::OrderIntent;
use kernel_core::clock::TimerKey;
use kernel_core::{DurationNanos, Result, Status, UnixNanos};
use model::account::AccountBalance;
use model::bar::BarType;
use model::enums::TradingState;
use model::instruments::Instrument;
use model::{ClientOrderId, Currency, InstrumentId, Price, Quantity};

use crate::kernel::Kernel;
use crate::views::{ExposureView, OrderView, PositionView};

/// A strategy's handle on the kernel, valid during one callback.
#[derive(Debug)]
pub struct Context<'a> {
    kernel: &'a mut Kernel,
    strategy: StrategyIndex,
}

impl<'a> Context<'a> {
    #[must_use]
    pub fn new(kernel: &'a mut Kernel, strategy: StrategyIndex) -> Self {
        Self { kernel, strategy }
    }

    #[must_use]
    pub const fn strategy(&self) -> StrategyIndex {
        self.strategy
    }

    // ---- time, randomness, records ------------------------------------------------------------

    /// The current input's time.
    #[must_use]
    pub const fn now(&self) -> UnixNanos {
        self.kernel.now()
    }
    /// A pure function of the seed, the current input and `key`: the same in a replay.
    #[must_use]
    pub const fn rng(&self, key: u32) -> u64 {
        self.kernel.rng(self.strategy, key)
    }
    /// Records a value (1e9 scale) under `tag`: a replay-checked output.
    pub fn record(&mut self, tag: &str, value: i64) -> Result<()> {
        self.kernel.record(self.strategy, tag, value)
    }

    // ---- timers -------------------------------------------------------------------------------

    /// A one-shot timer `id` at `deadline` (not in the past); a later call with the same id
    /// replaces it.
    pub fn set_timer(&mut self, id: u32, deadline: UnixNanos) -> Result<()> {
        self.kernel.set_timer(self.key(id), deadline, DurationNanos::default())
    }
    /// A periodic timer, first due at `first`, then every `period`.
    pub fn set_periodic_timer(
        &mut self,
        id: u32,
        first: UnixNanos,
        period: DurationNanos,
    ) -> Result<()> {
        if period.is_zero() {
            return Err(Status::InvalidArgument);
        }
        self.kernel.set_timer(self.key(id), first, period)
    }
    pub fn cancel_timer(&mut self, id: u32) -> Result<()> {
        self.kernel.cancel_timer(self.key(id))
    }
    const fn key(&self, id: u32) -> TimerKey {
        TimerKey::new(self.strategy.0 as u32, id)
    }

    // ---- subscriptions ------------------------------------------------------------------------

    pub fn subscribe(&mut self, kind: DataKind, id: &InstrumentId, cadence: Cadence) -> Result<()> {
        self.kernel.subscribe(self.strategy, kind, id, cadence)
    }
    pub fn subscribe_trades(&mut self, id: &InstrumentId, cadence: Cadence) -> Result<()> {
        self.subscribe(DataKind::Trade, id, cadence)
    }
    pub fn subscribe_quotes(&mut self, id: &InstrumentId, cadence: Cadence) -> Result<()> {
        self.subscribe(DataKind::Quote, id, cadence)
    }
    pub fn subscribe_book(&mut self, id: &InstrumentId, cadence: Cadence) -> Result<()> {
        self.subscribe(DataKind::Book, id, cadence)
    }
    pub fn subscribe_mark_price(&mut self, id: &InstrumentId, cadence: Cadence) -> Result<()> {
        self.subscribe(DataKind::MarkPrice, id, cadence)
    }
    pub fn subscribe_funding(&mut self, id: &InstrumentId, cadence: Cadence) -> Result<()> {
        self.subscribe(DataKind::FundingRate, id, cadence)
    }
    pub fn subscribe_bars(&mut self, bar_type: &BarType, cadence: Cadence) -> Result<()> {
        self.kernel.subscribe_bars(self.strategy, bar_type, cadence)
    }
    pub fn unsubscribe(&mut self, kind: DataKind, id: &InstrumentId) -> Result<()> {
        self.kernel.unsubscribe(self.strategy, kind, id)
    }
    pub fn unsubscribe_bars(&mut self, bar_type: &BarType) -> Result<()> {
        self.kernel.unsubscribe_bars(self.strategy, bar_type)
    }
    /// A kernel feature on `id`, delivered to `on_feature` at `cadence`.
    pub fn feature(
        &mut self,
        kind: FeatureKind,
        id: &InstrumentId,
        cadence: Cadence,
    ) -> Result<FeatureId> {
        self.kernel.declare_feature(self.strategy, kind, id, cadence)
    }

    // ---- orders -------------------------------------------------------------------------------

    /// Submits an intent; the id of the order, which is submitted or denied (an `OrderDenied`
    /// follows in `on_order_event`).
    pub fn submit(&mut self, intent: &OrderIntent) -> Result<ClientOrderId> {
        let slot = self.kernel.instruments.slot_of(&intent.instrument_id);
        let k = &mut *self.kernel;
        k.trading.submit(k.current, self.strategy, intent, slot, &k.instruments, &mut k.outputs)
    }
    /// A `LIMIT` order's new quantity and/or price.
    pub fn modify(
        &mut self,
        id: &ClientOrderId,
        quantity: Option<Quantity>,
        price: Option<Price>,
    ) -> Result<()> {
        let k = &mut *self.kernel;
        k.trading.modify(
            k.current,
            self.strategy,
            id,
            quantity,
            price,
            &k.instruments,
            &mut k.outputs,
        )
    }
    pub fn cancel(&mut self, id: &ClientOrderId) -> Result<()> {
        let k = &mut *self.kernel;
        k.trading.cancel(k.current, self.strategy, id, &mut k.outputs)
    }
    /// Cancels every cancelable order of this strategy (of one instrument when given); how many.
    pub fn cancel_all(&mut self, id: Option<&InstrumentId>) -> Result<u32> {
        let k = &mut *self.kernel;
        k.trading.cancel_all(k.current, self.strategy, id, &mut k.outputs)
    }

    // ---- queries ------------------------------------------------------------------------------

    #[must_use]
    pub fn instrument(&self, id: &InstrumentId) -> Option<&Instrument> {
        self.kernel.instrument(id)
    }
    /// The instrument's book, once subscribed and updated. `None` during the `on_book` callback
    /// of that very book (it is on loan to the callback).
    #[must_use]
    pub fn book(&self, id: &InstrumentId) -> Option<BookView<'_>> {
        let slot = self.kernel.instruments.slot_of(id)?;
        self.kernel.books.get(slot.index())?.as_ref().map(data::book::OrderBook::view)
    }
    #[must_use]
    pub fn order(&self, id: &ClientOrderId) -> Option<OrderView> {
        self.kernel.trading.order(self.strategy, id, &self.kernel.instruments)
    }
    /// Open orders of this strategy (of one instrument when given), oldest slot first; writes up
    /// to `out.len()` and returns how many there are.
    pub fn open_orders(&self, id: Option<&InstrumentId>, out: &mut [OrderView]) -> usize {
        self.kernel.trading.open_orders(self.strategy, id, &self.kernel.instruments, out)
    }
    #[must_use]
    pub fn position(&self, id: &InstrumentId) -> Option<PositionView> {
        let slot = self.slot(id)?;
        self.kernel.trading.position(self.strategy, slot, &self.kernel.instruments)
    }
    #[must_use]
    pub fn exposure(&self, id: &InstrumentId) -> Option<ExposureView> {
        let slot = self.slot(id)?;
        self.kernel.trading.exposure(slot, &self.kernel.instruments)
    }
    #[must_use]
    pub fn balance(&self, currency: Currency) -> Option<AccountBalance> {
        self.kernel.trading.balance(currency, &self.kernel.instruments)
    }
    #[must_use]
    pub fn trading_state(&self) -> TradingState {
        self.kernel.trading.risk.trading_state()
    }

    fn slot(&self, id: &InstrumentId) -> Option<InstrumentSlot> {
        self.kernel.instruments.slot_of(id)
    }
}
