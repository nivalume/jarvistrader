//! Structural checks every order intent passes before the rule catalog of docs/architecture.md
//! section 10.1 sees it: the order is well formed for its instrument (supported type and time in
//! force, precisions, a price exactly when the type needs one). A failed check denies the order
//! with one of the reason codes below, which go into `OrderDenied.reason` (`CATEGORY_CONDITION`).

use execution::OrderIntent;
use kernel_core::UnixNanos;
use model::enums::{OrderType, TimeInForce};
use model::instruments::Instrument;
use model::{Price, Quantity};

/// A reason code (`OrderDenied.reason`).
pub type ReasonCode = &'static str;

pub const INSTRUMENT_UNKNOWN: ReasonCode = "INSTRUMENT_UNKNOWN";
pub const ORDER_TYPE_UNSUPPORTED: ReasonCode = "ORDER_TYPE_UNSUPPORTED";
pub const TIME_IN_FORCE_UNSUPPORTED: ReasonCode = "TIME_IN_FORCE_UNSUPPORTED";
pub const POST_ONLY_INVALID: ReasonCode = "POST_ONLY_INVALID";
pub const QUANTITY_NOT_POSITIVE: ReasonCode = "QUANTITY_NOT_POSITIVE";
pub const QUANTITY_INVALID_PRECISION: ReasonCode = "QUANTITY_INVALID_PRECISION";
pub const PRICE_MISSING: ReasonCode = "PRICE_MISSING";
pub const PRICE_UNEXPECTED: ReasonCode = "PRICE_UNEXPECTED";
pub const PRICE_NOT_POSITIVE: ReasonCode = "PRICE_NOT_POSITIVE";
pub const PRICE_INVALID_PRECISION: ReasonCode = "PRICE_INVALID_PRECISION";
pub const GTD_EXPIRE_TIME_MISSING: ReasonCode = "GTD_EXPIRE_TIME_MISSING";
pub const GTD_ALREADY_EXPIRED: ReasonCode = "GTD_ALREADY_EXPIRED";
pub const OMS_CAPACITY_EXCEEDED: ReasonCode = "OMS_CAPACITY_EXCEEDED";
pub const STRATEGY_DISABLED: ReasonCode = "STRATEGY_DISABLED";
pub const ORDER_UNKNOWN: ReasonCode = "ORDER_UNKNOWN";
pub const ORDER_NOT_OPEN: ReasonCode = "ORDER_NOT_OPEN";
pub const ORDER_NOT_MODIFIABLE: ReasonCode = "ORDER_NOT_MODIFIABLE";

/// v1.0 orders: `MARKET` and `LIMIT` with `GTC`, `IOC`, `FOK` or `GTD`.
#[must_use]
pub const fn supported_time_in_force(tif: TimeInForce) -> bool {
    matches!(tif, TimeInForce::Gtc | TimeInForce::Ioc | TimeInForce::Fok | TimeInForce::Gtd)
}

#[must_use]
pub fn check_price(instrument: &Instrument, price: Price) -> Option<ReasonCode> {
    if !price.is_positive() {
        return Some(PRICE_NOT_POSITIVE);
    }
    if price.precision() != instrument.price_precision {
        return Some(PRICE_INVALID_PRECISION);
    }
    None
}

#[must_use]
pub fn check_quantity(instrument: &Instrument, quantity: Quantity) -> Option<ReasonCode> {
    if quantity.is_zero() {
        return Some(QUANTITY_NOT_POSITIVE);
    }
    if quantity.precision() != instrument.size_precision {
        return Some(QUANTITY_INVALID_PRECISION);
    }
    None
}

/// `None` when the intent is well formed.
#[must_use]
pub fn check_intent(
    instrument: &Instrument,
    intent: &OrderIntent,
    now: UnixNanos,
) -> Option<ReasonCode> {
    let limit = intent.order_type == OrderType::Limit;
    if !limit && intent.order_type != OrderType::Market {
        return Some(ORDER_TYPE_UNSUPPORTED);
    }
    if !supported_time_in_force(intent.time_in_force) {
        return Some(TIME_IN_FORCE_UNSUPPORTED);
    }
    // Post-only rests or is rejected: LIMIT only, and never IOC or FOK.
    if intent.post_only
        && (!limit || matches!(intent.time_in_force, TimeInForce::Ioc | TimeInForce::Fok))
    {
        return Some(POST_ONLY_INVALID);
    }
    if let Some(r) = check_quantity(instrument, intent.quantity) {
        return Some(r);
    }
    match (limit, intent.price) {
        (true, None) => return Some(PRICE_MISSING),
        (false, Some(_)) => return Some(PRICE_UNEXPECTED),
        (_, Some(p)) => {
            if let Some(r) = check_price(instrument, p) {
                return Some(r);
            }
        }
        (false, None) => {}
    }
    if intent.time_in_force == TimeInForce::Gtd {
        match intent.expire_time {
            None => return Some(GTD_EXPIRE_TIME_MISSING),
            Some(t) if t <= now => return Some(GTD_ALREADY_EXPIRED),
            Some(_) => {}
        }
    }
    None
}
