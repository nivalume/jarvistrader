//! The return path of docs/architecture.md section 9.1: an order event from the venue (the
//! adapter in live, the simulated exchange in backtest) advances the order in the OMS. Only events
//! the OMS accepts reach the strategy; the others are counted and dropped:
//!
//! - `UnknownOrder`: no order with this `ClientOrderId` (never ours, or evicted long ago);
//! - `Refused`: the state machine has no such transition, or the quantities do not fit (an
//!   overfill, a void of more than was filled);
//! - `DuplicateFill`: a second report of a trade already applied (`TRADE_LITE` and
//!   `ORDER_TRADE_UPDATE`, section 8.3); the caller books its commission when the first report
//!   was Lite ([`Oms::take_pending_commission`]);
//! - `Stale`: a status event older (by venue time) than the latest venue event applied to the
//!   order, delivered out of order: applying it would undo a newer change. Fills are never stale:
//!   a trade counts whatever its time, once.
//!
//! `OrderInitialized` is not accepted from outside: orders enter the OMS through submit, and
//! adopting a venue order is reconciliation's (section 15).

use kernel_core::Status;
use model::order_events::{FillInfoFlags, OrderEvent};
use model::VenueOrderId;

use crate::fsm::OrderEventKind;
use crate::oms::{Oms, OrderIndex};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventOutcome {
    Applied,
    UnknownOrder,
    Refused,
    DuplicateFill,
    Stale,
}

/// The venue order id an event carries, if any.
fn venue_order_id(event: &OrderEvent) -> Option<&VenueOrderId> {
    match event {
        OrderEvent::Accepted(e) => Some(&e.venue_order_id),
        OrderEvent::Filled(e) => Some(&e.venue_order_id),
        OrderEvent::FillVoided(e) => Some(&e.venue_order_id),
        OrderEvent::Canceled(e) => e.venue_order_id.as_ref(),
        OrderEvent::Expired(e) => e.venue_order_id.as_ref(),
        OrderEvent::Triggered(e) => e.venue_order_id.as_ref(),
        OrderEvent::PendingUpdate(e) => e.venue_order_id.as_ref(),
        OrderEvent::PendingCancel(e) => e.venue_order_id.as_ref(),
        OrderEvent::ModifyRejected(e) => e.venue_order_id.as_ref(),
        OrderEvent::CancelRejected(e) => e.venue_order_id.as_ref(),
        OrderEvent::Updated(e) => e.venue_order_id.as_ref(),
        OrderEvent::Initialized(_)
        | OrderEvent::Denied(_)
        | OrderEvent::Emulated(_)
        | OrderEvent::Released(_)
        | OrderEvent::Submitted(_)
        | OrderEvent::Rejected(_) => None,
    }
}

/// Applies `event` to the OMS; the index is the order's slot when the order is known.
pub fn apply_order_event(oms: &mut Oms, event: &OrderEvent) -> (EventOutcome, Option<OrderIndex>) {
    let Some(index) = oms.find(&event.header().client_order_id) else {
        return (EventOutcome::UnknownOrder, None);
    };
    let Some(kind) = OrderEventKind::of(event) else {
        return (EventOutcome::Refused, Some(index));
    };
    let ts = event.header().ts_event;
    let fill = matches!(kind, OrderEventKind::Filled | OrderEventKind::FillVoided);
    if !fill && oms.get(index).is_some_and(|r| ts < r.ts_venue) {
        return (EventOutcome::Stale, Some(index));
    }
    let applied = match event {
        OrderEvent::Filled(e) => {
            let lite = e.info_flags.is_set(FillInfoFlags::TRADE_LITE);
            oms.fill(index, &e.trade_id, e.last_qty, e.last_px, lite)
        }
        OrderEvent::FillVoided(e) => oms.void_fill(index, &e.trade_id, e.voided_qty, e.last_px),
        OrderEvent::Updated(e) => oms.update(index, e.quantity, e.price),
        _ => oms.apply(index, kind),
    };
    match applied {
        Err(Status::DuplicateFill) => return (EventOutcome::DuplicateFill, Some(index)),
        Err(_) => return (EventOutcome::Refused, Some(index)),
        Ok(()) => {}
    }
    if let Some(r) = oms.get_mut(index) {
        // The first venue order id an event carries is kept.
        if r.venue_order_id.is_none() {
            if let Some(v) = venue_order_id(event).filter(|v| !v.as_str().is_empty()) {
                r.venue_order_id = Some(*v);
            }
        }
        if ts > r.ts_venue {
            r.ts_venue = ts;
        }
    }
    (EventOutcome::Applied, Some(index))
}
