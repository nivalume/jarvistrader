//! The state of one order under the apply-phase rules of `specs/tla/OrderLifecycle.tla`
//! (docs/architecture.md section 8.1): the transition table decides the next status, then
//!
//! - a fill makes the order `Filled` when it completes it, keeps `Canceled` after a cancel it
//!   overtook, and otherwise makes it `PartiallyFilled`, except that a pending update or cancel
//!   stays pending and remembers `PartiallyFilled` as the status to return to;
//! - `ModifyRejected` (unless a cancel is pending), `CancelRejected` while a cancel is pending,
//!   and `Updated` while a request is pending restore the previous status;
//! - the previous status is saved on every transition except the rejections and while a
//!   request is already pending.
//!
//! Duplicate fills and voids of unknown trades are the OMS's to refuse (it owns the trade
//! records); this type checks quantities and transitions.

use kernel_core::state::{State, StateReader, StateWriter};
use kernel_core::{Result, Status};
use model::enums::OrderStatus;
use model::Quantity;

use crate::fsm::{is_pending, next_status, OrderEventKind};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OrderState {
    status: OrderStatus,
    previous: Option<OrderStatus>,
    quantity: Quantity,
    filled: Quantity,
}

impl Default for OrderState {
    fn default() -> Self {
        Self::new(Quantity::default())
    }
}

impl OrderState {
    /// A new order (`Initialized`) of `quantity`.
    #[must_use]
    pub fn new(quantity: Quantity) -> Self {
        Self {
            status: OrderStatus::Initialized,
            previous: None,
            quantity,
            filled: zero_like(quantity),
        }
    }

    /// An order in a given state (recovery and exhaustive tests). `filled` must not exceed
    /// `quantity`; both carry the same precision.
    pub fn restore(
        status: OrderStatus,
        previous: Option<OrderStatus>,
        quantity: Quantity,
        filled: Quantity,
    ) -> Result<Self> {
        if filled.raw() > quantity.raw() || filled.precision() != quantity.precision() {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { status, previous, quantity, filled })
    }

    #[must_use]
    pub const fn status(&self) -> OrderStatus {
        self.status
    }
    #[must_use]
    pub const fn previous(&self) -> Option<OrderStatus> {
        self.previous
    }
    #[must_use]
    pub const fn quantity(&self) -> Quantity {
        self.quantity
    }
    #[must_use]
    pub const fn filled(&self) -> Quantity {
        self.filled
    }
    #[must_use]
    pub fn leaves(&self) -> Quantity {
        Quantity::from_raw(self.quantity.raw() - self.filled.raw(), self.quantity.precision())
            .unwrap_or_default()
    }
    #[must_use]
    pub const fn leaves_raw(&self) -> u64 {
        self.quantity.raw() - self.filled.raw()
    }

    /// An event without a quantity (`InvalidArgument` for the three that carry one).
    pub fn apply(&mut self, kind: OrderEventKind) -> Result<()> {
        if kind.has_quantity() {
            return Err(Status::InvalidArgument);
        }
        let to = next_status(self.status, kind).ok_or(Status::InvalidTransition)?;
        let restore = (kind == OrderEventKind::ModifyRejected
            && self.status != OrderStatus::PendingCancel)
            || (kind == OrderEventKind::CancelRejected
                && self.status == OrderStatus::PendingCancel);
        let back = match (restore, self.previous) {
            (true, Some(p)) => p,
            (true, None) => return Err(Status::InvalidState),
            (false, _) => to,
        };
        self.save_previous(kind);
        self.status = back;
        Ok(())
    }

    /// `OrderUpdated` with the order's new quantity, which must exceed what is filled.
    pub fn update(&mut self, quantity: Quantity) -> Result<()> {
        let to =
            next_status(self.status, OrderEventKind::Updated).ok_or(Status::InvalidTransition)?;
        if quantity.raw() <= self.filled.raw() || quantity.precision() != self.quantity.precision()
        {
            return Err(Status::InvalidArgument);
        }
        let back = match (is_pending(self.status), self.previous) {
            (true, Some(p)) => p,
            _ => to,
        };
        self.save_previous(OrderEventKind::Updated);
        self.status = back;
        self.quantity = quantity;
        Ok(())
    }

    /// `OrderFilled` for `qty`, at most the leaves quantity (the OMS has refused duplicates).
    pub fn fill(&mut self, qty: Quantity) -> Result<()> {
        next_status(self.status, OrderEventKind::Filled).ok_or(Status::InvalidTransition)?;
        if qty.is_zero()
            || qty.raw() > self.leaves_raw()
            || qty.precision() != self.quantity.precision()
        {
            return Err(Status::InvalidArgument);
        }
        let filled = self.filled.raw() + qty.raw();
        if filled >= self.quantity.raw() {
            self.save_previous(OrderEventKind::Filled);
            self.status = OrderStatus::Filled;
        } else if self.status == OrderStatus::Canceled {
            self.save_previous(OrderEventKind::Filled);
        } else if is_pending(self.status) {
            self.previous = Some(OrderStatus::PartiallyFilled);
        } else {
            self.save_previous(OrderEventKind::Filled);
            self.status = OrderStatus::PartiallyFilled;
        }
        self.filled = Quantity::from_raw(filled, self.quantity.precision())?;
        Ok(())
    }

    /// `OrderFillVoided` for `voided` of an earlier fill (the OMS checks the trade and its size).
    pub fn void_fill(&mut self, voided: Quantity) -> Result<()> {
        next_status(self.status, OrderEventKind::FillVoided).ok_or(Status::InvalidTransition)?;
        if voided.is_zero()
            || voided.raw() > self.filled.raw()
            || voided.precision() != self.quantity.precision()
        {
            return Err(Status::InvalidArgument);
        }
        let filled = self.filled.raw() - voided.raw();
        let source = self.status;
        self.save_previous(OrderEventKind::FillVoided);
        self.status = match source {
            OrderStatus::Filled => OrderStatus::Voided,
            OrderStatus::Canceled
            | OrderStatus::Expired
            | OrderStatus::Voided
            | OrderStatus::Triggered
            | OrderStatus::PendingUpdate
            | OrderStatus::PendingCancel => source,
            _ if filled == 0 => OrderStatus::Accepted,
            _ => OrderStatus::PartiallyFilled,
        };
        self.filled = Quantity::from_raw(filled, self.quantity.precision())?;
        Ok(())
    }

    fn save_previous(&mut self, kind: OrderEventKind) {
        if kind != OrderEventKind::ModifyRejected
            && kind != OrderEventKind::CancelRejected
            && !is_pending(self.status)
        {
            self.previous = Some(self.status);
        }
    }
}

fn zero_like(q: Quantity) -> Quantity {
    Quantity::from_raw(0, q.precision()).unwrap_or_default()
}

impl State for OrderState {
    fn write(&self, w: &mut StateWriter<'_>) {
        self.status.write(w);
        self.previous.write(w);
        self.quantity.write(w);
        self.filled.write(w);
    }
    fn read(&mut self, r: &mut StateReader<'_>) {
        self.status.read(r);
        self.previous.read(r);
        self.quantity.read(r);
        self.filled.read(r);
        if self.filled.raw() > self.quantity.raw()
            || self.filled.precision() != self.quantity.precision()
        {
            r.fail(Status::InvalidState);
        }
    }
}
