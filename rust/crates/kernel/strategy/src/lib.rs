//! Kernel layer `strategy` (docs/architecture.md sections 7 and 9): the [`Strategy`] trait, the
//! [`Context`] a callback receives, and the kernel services the engine and the contexts share
//! ([`Kernel`], [`Trading`]). A strategy is any type implementing the trait; the callbacks it does
//! not override are no-ops. Callbacks return `Result<()>`; a failure disables the strategy
//! (`ErrorPolicy`).
#![no_std]
#![forbid(unsafe_code)]
#![deny(clippy::float_arithmetic)]

extern crate alloc;

pub mod context;
pub mod kernel;
pub mod trading;
pub mod views;

use data::book::BookView;
use data::{FeatureId, FeatureValue};
use kernel_core::clock::TimerKey;
use kernel_core::state::{StateReader, StateWriter};
use kernel_core::{Result, UnixNanos};
use model::bar::Bar;
use model::data::{
    FundingRateUpdate, IndexPriceUpdate, InstrumentClose, InstrumentStatus, LiquidationOrder,
    MarkPriceUpdate, OrderBookDeltas, QuoteTick, TradeTick,
};
use model::instruments::Instrument;
use model::order_events::OrderEvent;
use model::position_events::PositionEvent;
use model::InstrumentId;

pub use context::Context;
pub use data::subscription::{Cadence, DataKind, StrategyIndex};
pub use execution::OrderIntent;
pub use kernel::{ErrorPolicy, Kernel, KernelConfig, StrategyFailure};
pub use trading::{Trading, TradingConfig};
pub use views::{ExposureView, OrderView, PositionView};

/// A strategy's callbacks. Every method has a no-op default.
#[allow(unused_variables)]
pub trait Strategy {
    /// The node enters `Running`.
    fn on_start(&mut self, ctx: &mut Context<'_>) -> Result<()> {
        Ok(())
    }
    /// The node enters `Stopping`; no callback follows.
    fn on_stop(&mut self, ctx: &mut Context<'_>) -> Result<()> {
        Ok(())
    }
    fn on_trade(&mut self, ctx: &mut Context<'_>, trade: &TradeTick) -> Result<()> {
        Ok(())
    }
    fn on_quote(&mut self, ctx: &mut Context<'_>, quote: &QuoteTick) -> Result<()> {
        Ok(())
    }
    /// The book after an update.
    fn on_book(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        book: &BookView<'_>,
    ) -> Result<()> {
        Ok(())
    }
    fn on_book_deltas(&mut self, ctx: &mut Context<'_>, deltas: &OrderBookDeltas) -> Result<()> {
        Ok(())
    }
    fn on_bar(&mut self, ctx: &mut Context<'_>, bar: &Bar) -> Result<()> {
        Ok(())
    }
    fn on_mark_price(&mut self, ctx: &mut Context<'_>, update: &MarkPriceUpdate) -> Result<()> {
        Ok(())
    }
    fn on_index_price(&mut self, ctx: &mut Context<'_>, update: &IndexPriceUpdate) -> Result<()> {
        Ok(())
    }
    fn on_funding_rate(&mut self, ctx: &mut Context<'_>, update: &FundingRateUpdate) -> Result<()> {
        Ok(())
    }
    fn on_instrument_status(
        &mut self,
        ctx: &mut Context<'_>,
        status: &InstrumentStatus,
    ) -> Result<()> {
        Ok(())
    }
    fn on_instrument_close(
        &mut self,
        ctx: &mut Context<'_>,
        close: &InstrumentClose,
    ) -> Result<()> {
        Ok(())
    }
    fn on_liquidation(&mut self, ctx: &mut Context<'_>, order: &LiquidationOrder) -> Result<()> {
        Ok(())
    }
    fn on_instrument(&mut self, ctx: &mut Context<'_>, instrument: &Instrument) -> Result<()> {
        Ok(())
    }
    fn on_feature(
        &mut self,
        ctx: &mut Context<'_>,
        id: FeatureId,
        value: FeatureValue,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<()> {
        Ok(())
    }
    /// All trades of one `OnBatch` subscription within one batch.
    fn on_trade_batch(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        trades: &[TradeTick],
    ) -> Result<()> {
        Ok(())
    }
    fn on_quote_batch(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        quotes: &[QuoteTick],
    ) -> Result<()> {
        Ok(())
    }
    /// Every event of this strategy's orders, from the kernel (submitted, denied, pending) or
    /// the venue.
    fn on_order_event(&mut self, ctx: &mut Context<'_>, event: &OrderEvent) -> Result<()> {
        Ok(())
    }
    /// This strategy's positions: opened, changed, closed, adjusted (funding).
    fn on_position_event(&mut self, ctx: &mut Context<'_>, event: &PositionEvent) -> Result<()> {
        Ok(())
    }
    fn on_timer(
        &mut self,
        ctx: &mut Context<'_>,
        key: TimerKey,
        deadline: UnixNanos,
    ) -> Result<()> {
        Ok(())
    }

    /// Whether the strategy describes its own state for snapshots (`save_state`, `load_state`).
    /// Without it a snapshot still holds the kernel's state, and a node recovers the strategy by
    /// replaying the log instead.
    fn has_state(&self) -> bool {
        false
    }
    fn save_state(&self, w: &mut StateWriter<'_>) {}
    fn load_state(&mut self, r: &mut StateReader<'_>) {}
}
