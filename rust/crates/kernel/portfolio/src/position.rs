//! A netting position in integers:
//!
//! - signed quantity: raw at 10^9, positive long and negative short;
//! - open notional: the sum of `price.raw x quantity.raw` over the open quantity (10^18 scale), so
//!   the average open price is exact and a full close realizes exactly the difference between the
//!   exit and entry notionals;
//! - PnL: raw at 10^9 in the settlement currency, for linear contracts:
//!   `(exit - entry) x quantity x multiplier`, sign by side.
//!
//! [`NettingPosition::apply`] takes at most what closes the position; the caller splits a fill that
//! flips it (closing part, then opening part) so each part produces its own position event.

use kernel_core::{Result, Status, UnixNanos};
use model::enums::{OrderSide, PositionSide};
use model::{ClientOrderId, Price, Quantity, FIXED_PRECISION, FIXED_SCALAR};

const SCALE2: i128 = (FIXED_SCALAR as i128) * (FIXED_SCALAR as i128);

/// A signed 10^18-scale amount times a 10^9-scale multiplier, back to 10^9, truncated toward zero.
fn settle(amount_18: i128, multiplier_raw: u64) -> Result<i64> {
    // |amount| < 2^127 and multiplier < 2^64 can overflow i128; split the multiplier.
    let m = i128::from(multiplier_raw);
    let whole = amount_18 / SCALE2 * m;
    let rest = amount_18 % SCALE2 * m / SCALE2;
    i64::try_from(whole + rest).map_err(|_| Status::Overflow)
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NettingPosition {
    signed_raw: i64,
    open_notional: u128,
    close_notional: u128,
    closed_raw: u64,
    peak_raw: u64,
    entry: Option<OrderSide>,
    realized_raw: i64,
    commission_raw: i64,
    funding_raw: i64,
    total_realized_raw: i64,
    total_commission_raw: i64,
    total_funding_raw: i64,
    opening_order_id: Option<ClientOrderId>,
    ts_opened: UnixNanos,
    ts_last: UnixNanos,
    last_px: Price,
    last_qty: Quantity,
    avg_px_open_raw: Option<i64>,
}

kernel_core::state_fields!(NettingPosition {
    signed_raw,
    open_notional,
    close_notional,
    closed_raw,
    peak_raw,
    entry,
    realized_raw,
    commission_raw,
    funding_raw,
    total_realized_raw,
    total_commission_raw,
    total_funding_raw,
    opening_order_id,
    ts_opened,
    ts_last,
    last_px,
    last_qty,
    avg_px_open_raw
});

/// What a fill did to the position.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Applied {
    /// PnL this fill realized (10^9 raw, settlement currency); zero for an opening fill.
    pub realized_raw: i64,
    pub opened: bool,
    pub closed: bool,
}

impl NettingPosition {
    /// The quantity a fill on `side` can close (0 when it would add to the position).
    #[must_use]
    pub const fn closable(&self, side: OrderSide) -> u64 {
        match (side, self.signed_raw > 0) {
            (OrderSide::Sell, true) | (OrderSide::Buy, false) => self.signed_raw.unsigned_abs(),
            _ => 0,
        }
    }

    /// Applies a fill of `quantity` at `price`. When the fill reduces the position it must not
    /// exceed [`closable`](Self::closable): the caller splits a flip.
    pub fn apply(
        &mut self,
        side: OrderSide,
        quantity: Quantity,
        price: Price,
        multiplier_raw: u64,
        order_id: ClientOrderId,
        ts: UnixNanos,
    ) -> Result<Applied> {
        if quantity.is_undef() || quantity.is_zero() || price.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let qty = quantity.raw();
        let notional = u128::from(price.raw().unsigned_abs()) * u128::from(qty);
        let reduces = self.closable(side) > 0;
        let mut applied = Applied { realized_raw: 0, opened: false, closed: false };
        if reduces {
            let held = self.signed_raw.unsigned_abs();
            if qty > held {
                return Err(Status::InvalidArgument);
            }
            // Remove entry cost in proportion; the last unit takes the whole remainder.
            let removed = if qty == held {
                self.open_notional
            } else {
                self.open_notional * u128::from(qty) / u128::from(held)
            };
            let diff = i128::try_from(notional).map_err(|_| Status::Overflow)?
                - i128::try_from(removed).map_err(|_| Status::Overflow)?;
            let signed_diff = if self.signed_raw > 0 { diff } else { -diff };
            let realized = settle(signed_diff, multiplier_raw)?;
            self.open_notional -= removed;
            self.close_notional += notional;
            self.closed_raw += qty;
            self.signed_raw = if self.signed_raw > 0 {
                self.signed_raw - qty as i64
            } else {
                self.signed_raw + qty as i64
            };
            self.realized_raw = self.realized_raw.checked_add(realized).ok_or(Status::Overflow)?;
            self.total_realized_raw =
                self.total_realized_raw.checked_add(realized).ok_or(Status::Overflow)?;
            applied.realized_raw = realized;
            applied.closed = self.signed_raw == 0;
        } else {
            if self.signed_raw == 0 {
                self.begin(side, order_id, ts);
                applied.opened = true;
            }
            let qty_i = i64::try_from(qty).map_err(|_| Status::Overflow)?;
            self.signed_raw = match side {
                OrderSide::Buy => self.signed_raw.checked_add(qty_i),
                OrderSide::Sell => self.signed_raw.checked_sub(qty_i),
            }
            .ok_or(Status::Overflow)?;
            self.open_notional += notional;
            self.peak_raw = self.peak_raw.max(self.signed_raw.unsigned_abs());
            self.avg_px_open_raw = Some(
                i64::try_from(self.open_notional / u128::from(self.signed_raw.unsigned_abs()))
                    .map_err(|_| Status::Overflow)?,
            );
        }
        self.ts_last = ts;
        self.last_px = price;
        self.last_qty = quantity;
        Ok(applied)
    }

    fn begin(&mut self, side: OrderSide, order_id: ClientOrderId, ts: UnixNanos) {
        self.entry = Some(side);
        self.opening_order_id = Some(order_id);
        self.ts_opened = ts;
        self.open_notional = 0;
        self.close_notional = 0;
        self.closed_raw = 0;
        self.peak_raw = 0;
        self.realized_raw = 0;
        self.commission_raw = 0;
        self.funding_raw = 0;
        self.avg_px_open_raw = None;
    }

    /// Replaces the position with `signed_raw` held at `avg_px` (reconciliation sets the venue's
    /// position). The totals since the node started stay; the per-position figures restart as for
    /// a newly opened position.
    pub fn set(&mut self, signed_raw: i64, avg_px: Price, ts: UnixNanos) -> Result<()> {
        if avg_px.is_undef() || avg_px.raw() < 0 {
            return Err(Status::InvalidArgument);
        }
        let side = if signed_raw >= 0 { OrderSide::Buy } else { OrderSide::Sell };
        self.begin(side, ClientOrderId::new("EXTERNAL")?, ts);
        self.signed_raw = signed_raw;
        self.open_notional =
            u128::from(avg_px.raw().unsigned_abs()) * u128::from(signed_raw.unsigned_abs());
        self.peak_raw = signed_raw.unsigned_abs();
        self.avg_px_open_raw = (signed_raw != 0).then_some(avg_px.raw());
        if signed_raw == 0 {
            self.entry = None;
        }
        self.ts_last = ts;
        Ok(())
    }

    /// Commission paid (positive) or a rebate (negative), in the settlement currency.
    pub fn add_commission(&mut self, raw: i64) -> Result<()> {
        self.commission_raw = self.commission_raw.checked_add(raw).ok_or(Status::Overflow)?;
        self.total_commission_raw =
            self.total_commission_raw.checked_add(raw).ok_or(Status::Overflow)?;
        Ok(())
    }
    /// Funding received (positive) or paid (negative), in the settlement currency.
    pub fn add_funding(&mut self, raw: i64) -> Result<()> {
        self.funding_raw = self.funding_raw.checked_add(raw).ok_or(Status::Overflow)?;
        self.total_funding_raw = self.total_funding_raw.checked_add(raw).ok_or(Status::Overflow)?;
        Ok(())
    }

    /// PnL of the open quantity at `mark` (10^9 raw, settlement currency).
    pub fn unrealized(&self, mark: Price, multiplier_raw: u64) -> Result<i64> {
        if self.signed_raw == 0 {
            return Ok(0);
        }
        if mark.is_undef() {
            return Err(Status::InvalidArgument);
        }
        let at_mark = i128::from(mark.raw()) * i128::from(self.signed_raw.unsigned_abs());
        let diff = at_mark - i128::try_from(self.open_notional).map_err(|_| Status::Overflow)?;
        settle(if self.signed_raw > 0 { diff } else { -diff }, multiplier_raw)
    }

    #[must_use]
    pub const fn signed_raw(&self) -> i64 {
        self.signed_raw
    }
    #[must_use]
    pub const fn quantity_raw(&self) -> u64 {
        self.signed_raw.unsigned_abs()
    }
    /// The open quantity at full precision.
    pub fn quantity(&self) -> Result<Quantity> {
        Quantity::from_raw(self.quantity_raw(), FIXED_PRECISION)
    }
    #[must_use]
    pub const fn is_open(&self) -> bool {
        self.signed_raw != 0
    }
    #[must_use]
    pub const fn side(&self) -> PositionSide {
        if self.signed_raw > 0 {
            PositionSide::Long
        } else if self.signed_raw < 0 {
            PositionSide::Short
        } else {
            PositionSide::Flat
        }
    }
    #[must_use]
    pub const fn entry(&self) -> Option<OrderSide> {
        self.entry
    }
    #[must_use]
    pub const fn open_notional(&self) -> u128 {
        self.open_notional
    }
    #[must_use]
    pub const fn peak_raw(&self) -> u64 {
        self.peak_raw
    }
    #[must_use]
    pub const fn realized_raw(&self) -> i64 {
        self.realized_raw
    }
    #[must_use]
    pub const fn commission_raw(&self) -> i64 {
        self.commission_raw
    }
    #[must_use]
    pub const fn funding_raw(&self) -> i64 {
        self.funding_raw
    }
    /// Since the node started, across every opening of this position.
    #[must_use]
    pub const fn total_realized_raw(&self) -> i64 {
        self.total_realized_raw
    }
    #[must_use]
    pub const fn total_commission_raw(&self) -> i64 {
        self.total_commission_raw
    }
    #[must_use]
    pub const fn total_funding_raw(&self) -> i64 {
        self.total_funding_raw
    }
    /// The order whose fill opened the position; `EXTERNAL` when reconciliation set it; `None`
    /// before it ever opened.
    #[must_use]
    pub const fn opening_order_id(&self) -> Option<&ClientOrderId> {
        self.opening_order_id.as_ref()
    }
    #[must_use]
    pub const fn ts_opened(&self) -> UnixNanos {
        self.ts_opened
    }
    #[must_use]
    pub const fn ts_last(&self) -> UnixNanos {
        self.ts_last
    }
    #[must_use]
    pub const fn last_px(&self) -> Price {
        self.last_px
    }
    #[must_use]
    pub const fn last_qty(&self) -> Quantity {
        self.last_qty
    }

    /// Average price of the opening fills at full precision, kept after the position closes;
    /// `None` before it first opened.
    #[must_use]
    pub fn avg_px_open(&self) -> Option<Price> {
        self.avg_px_open_raw.and_then(|raw| Price::from_raw(raw, FIXED_PRECISION).ok())
    }
    /// Average price of the closing fills since the position opened; `None` before any.
    #[must_use]
    pub fn avg_px_close(&self) -> Option<Price> {
        if self.closed_raw == 0 {
            return None;
        }
        let raw = i64::try_from(self.close_notional / u128::from(self.closed_raw)).ok()?;
        Price::from_raw(raw, FIXED_PRECISION).ok()
    }
}
