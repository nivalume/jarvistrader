//! Routing (docs/architecture.md section 7.2): which subscription row and kind an input event is
//! delivered under. Instrument data is keyed by instrument slot, bars by bar key. Events that are
//! not market data have no route.

use kernel_core::Result;
use model::Event;

use crate::intern::{BarTable, InstrumentTable};
use crate::subscription::DataKind;

/// A cell of the subscription matrix.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Route {
    pub row: u32,
    pub kind: DataKind,
}

/// The route of `event`, interning its instrument id or bar type when first seen; `None` for
/// events that are not delivered to strategies by subscription (orders, account, time, control).
pub fn route_of(
    event: &Event,
    instruments: &mut InstrumentTable,
    bars: &mut BarTable,
) -> Result<Option<Route>> {
    let mut by_instrument = |id, kind| -> Result<Option<Route>> {
        Ok(Some(Route { row: instruments.intern(id)?.0, kind }))
    };
    match event {
        Event::TradeTick(e) => by_instrument(e.instrument_id, DataKind::Trade),
        Event::QuoteTick(e) => by_instrument(e.instrument_id, DataKind::Quote),
        Event::OrderBookDeltas(e) => by_instrument(e.instrument_id, DataKind::Book),
        Event::MarkPriceUpdate(e) => by_instrument(e.instrument_id, DataKind::MarkPrice),
        Event::IndexPriceUpdate(e) => by_instrument(e.instrument_id, DataKind::IndexPrice),
        Event::FundingRateUpdate(e) => by_instrument(e.instrument_id, DataKind::FundingRate),
        Event::InstrumentStatus(e) => by_instrument(e.instrument_id, DataKind::InstrumentStatus),
        Event::InstrumentClose(e) => by_instrument(e.instrument_id, DataKind::InstrumentClose),
        Event::LiquidationOrder(e) => by_instrument(e.instrument_id, DataKind::Liquidation),
        Event::Instrument(e) => by_instrument(e.id, DataKind::Instrument),
        Event::Bar(e) => Ok(Some(Route { row: bars.intern(e.bar_type)?, kind: DataKind::Bar })),
        Event::Order(_)
        | Event::AccountState(_)
        | Event::RateLimitFeedback(_)
        | Event::TimerFired(_)
        | Event::BatchEnd(_)
        | Event::Shutdown(_) => Ok(None),
    }
}
