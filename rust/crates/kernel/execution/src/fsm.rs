//! The order state machine's transition relation (docs/architecture.md section 8.1): nautilus
//! `OrderStatus::transition` at cd417b80 plus `SUBMITTED --EXPIRED--> EXPIRED`. The TLA+ spec
//! `specs/tla/OrderLifecycle.tla` is the source of truth; `tests/fsm.rs` reads the set between
//! `BEGIN TRANSITIONS` and `END TRANSITIONS` and fails when this table differs. The apply-phase
//! rules (fills, the previous status) live in [`crate::order`].

use model::enums::OrderStatus;
use model::order_events::OrderEvent;

/// Every order event except `OrderInitialized`, which creates the order instead of moving it.
/// The discriminants follow the spec's `Kinds` order.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum OrderEventKind {
    Denied = 0,
    Emulated = 1,
    Released = 2,
    Submitted = 3,
    Accepted = 4,
    Rejected = 5,
    Canceled = 6,
    Expired = 7,
    Triggered = 8,
    PendingUpdate = 9,
    PendingCancel = 10,
    ModifyRejected = 11,
    CancelRejected = 12,
    Updated = 13,
    Filled = 14,
    FillVoided = 15,
}

impl OrderEventKind {
    pub const ALL: [OrderEventKind; 16] = [
        OrderEventKind::Denied,
        OrderEventKind::Emulated,
        OrderEventKind::Released,
        OrderEventKind::Submitted,
        OrderEventKind::Accepted,
        OrderEventKind::Rejected,
        OrderEventKind::Canceled,
        OrderEventKind::Expired,
        OrderEventKind::Triggered,
        OrderEventKind::PendingUpdate,
        OrderEventKind::PendingCancel,
        OrderEventKind::ModifyRejected,
        OrderEventKind::CancelRejected,
        OrderEventKind::Updated,
        OrderEventKind::Filled,
        OrderEventKind::FillVoided,
    ];

    /// The spec's name (`specs/tla/OrderLifecycle.tla` `Kinds`).
    #[must_use]
    pub const fn spec_name(self) -> &'static str {
        match self {
            OrderEventKind::Denied => "DENIED",
            OrderEventKind::Emulated => "EMULATED",
            OrderEventKind::Released => "RELEASED",
            OrderEventKind::Submitted => "SUBMITTED",
            OrderEventKind::Accepted => "ACCEPTED",
            OrderEventKind::Rejected => "REJECTED",
            OrderEventKind::Canceled => "CANCELED",
            OrderEventKind::Expired => "EXPIRED",
            OrderEventKind::Triggered => "TRIGGERED",
            OrderEventKind::PendingUpdate => "PENDING_UPDATE",
            OrderEventKind::PendingCancel => "PENDING_CANCEL",
            OrderEventKind::ModifyRejected => "MODIFY_REJECTED",
            OrderEventKind::CancelRejected => "CANCEL_REJECTED",
            OrderEventKind::Updated => "UPDATED",
            OrderEventKind::Filled => "FILLED",
            OrderEventKind::FillVoided => "FILL_VOIDED",
        }
    }

    #[must_use]
    pub fn from_spec_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|k| k.spec_name() == name)
    }

    /// Whether the event carries quantities (`Updated`, `Filled`, `FillVoided`).
    #[must_use]
    pub const fn has_quantity(self) -> bool {
        matches!(
            self,
            OrderEventKind::Updated | OrderEventKind::Filled | OrderEventKind::FillVoided
        )
    }

    /// The kind of an event; `None` for `OrderInitialized`.
    #[must_use]
    pub const fn of(event: &OrderEvent) -> Option<Self> {
        Some(match event {
            OrderEvent::Initialized(_) => return None,
            OrderEvent::Denied(_) => OrderEventKind::Denied,
            OrderEvent::Emulated(_) => OrderEventKind::Emulated,
            OrderEvent::Released(_) => OrderEventKind::Released,
            OrderEvent::Submitted(_) => OrderEventKind::Submitted,
            OrderEvent::Accepted(_) => OrderEventKind::Accepted,
            OrderEvent::Rejected(_) => OrderEventKind::Rejected,
            OrderEvent::Canceled(_) => OrderEventKind::Canceled,
            OrderEvent::Expired(_) => OrderEventKind::Expired,
            OrderEvent::Triggered(_) => OrderEventKind::Triggered,
            OrderEvent::PendingUpdate(_) => OrderEventKind::PendingUpdate,
            OrderEvent::PendingCancel(_) => OrderEventKind::PendingCancel,
            OrderEvent::ModifyRejected(_) => OrderEventKind::ModifyRejected,
            OrderEvent::CancelRejected(_) => OrderEventKind::CancelRejected,
            OrderEvent::Updated(_) => OrderEventKind::Updated,
            OrderEvent::Filled(_) => OrderEventKind::Filled,
            OrderEvent::FillVoided(_) => OrderEventKind::FillVoided,
        })
    }
}

kernel_core::state_enum!(OrderEventKind: u8 {
    Denied, Emulated, Released, Submitted, Accepted, Rejected, Canceled, Expired, Triggered,
    PendingUpdate, PendingCancel, ModifyRejected, CancelRejected, Updated, Filled, FillVoided
});

/// `(from, kind, to)`.
pub type Transition = (OrderStatus, OrderEventKind, OrderStatus);

use OrderEventKind as K;
use OrderStatus as S;

/// The transition relation, in the spec's order.
#[rustfmt::skip]
pub const TRANSITIONS: [Transition; 82] = [
    (S::Initialized, K::Denied, S::Denied),
    (S::Initialized, K::Emulated, S::Emulated),
    (S::Initialized, K::Released, S::Released),
    (S::Initialized, K::Submitted, S::Submitted),
    (S::Initialized, K::Rejected, S::Rejected),
    (S::Initialized, K::Accepted, S::Accepted),
    (S::Initialized, K::Canceled, S::Canceled),
    (S::Initialized, K::Expired, S::Expired),
    (S::Initialized, K::Triggered, S::Triggered),
    (S::Initialized, K::Updated, S::Initialized),
    (S::Emulated, K::Canceled, S::Canceled),
    (S::Emulated, K::Expired, S::Expired),
    (S::Emulated, K::Updated, S::Emulated),
    (S::Emulated, K::Released, S::Released),
    (S::Released, K::Submitted, S::Submitted),
    (S::Released, K::Denied, S::Denied),
    (S::Released, K::Canceled, S::Canceled),
    (S::Released, K::Updated, S::Released),
    (S::Submitted, K::PendingUpdate, S::PendingUpdate),
    (S::Submitted, K::PendingCancel, S::PendingCancel),
    (S::Submitted, K::Rejected, S::Rejected),
    (S::Submitted, K::Canceled, S::Canceled),
    (S::Submitted, K::Expired, S::Expired),
    (S::Submitted, K::Accepted, S::Accepted),
    (S::Submitted, K::Updated, S::Submitted),
    (S::Submitted, K::Filled, S::Filled),
    (S::Accepted, K::Rejected, S::Rejected),
    (S::Accepted, K::PendingUpdate, S::PendingUpdate),
    (S::Accepted, K::PendingCancel, S::PendingCancel),
    (S::Accepted, K::CancelRejected, S::Accepted),
    (S::Accepted, K::Canceled, S::Canceled),
    (S::Accepted, K::Triggered, S::Triggered),
    (S::Accepted, K::Updated, S::Accepted),
    (S::Accepted, K::Expired, S::Expired),
    (S::Accepted, K::Filled, S::Filled),
    (S::Accepted, K::FillVoided, S::Accepted),
    (S::Canceled, K::Filled, S::Filled),
    (S::Canceled, K::FillVoided, S::Canceled),
    (S::Canceled, K::Updated, S::Canceled),
    (S::PendingUpdate, K::Rejected, S::Rejected),
    (S::PendingUpdate, K::Accepted, S::Accepted),
    (S::PendingUpdate, K::Canceled, S::Canceled),
    (S::PendingUpdate, K::Expired, S::Expired),
    (S::PendingUpdate, K::Triggered, S::Triggered),
    (S::PendingUpdate, K::Submitted, S::PendingUpdate),
    (S::PendingUpdate, K::PendingUpdate, S::PendingUpdate),
    (S::PendingUpdate, K::PendingCancel, S::PendingCancel),
    (S::PendingUpdate, K::ModifyRejected, S::PendingUpdate),
    (S::PendingUpdate, K::Updated, S::PendingUpdate),
    (S::PendingUpdate, K::Filled, S::Filled),
    (S::PendingUpdate, K::FillVoided, S::PendingUpdate),
    (S::PendingCancel, K::Rejected, S::Rejected),
    (S::PendingCancel, K::PendingCancel, S::PendingCancel),
    (S::PendingCancel, K::ModifyRejected, S::PendingCancel),
    (S::PendingCancel, K::CancelRejected, S::PendingCancel),
    (S::PendingCancel, K::Canceled, S::Canceled),
    (S::PendingCancel, K::Expired, S::Expired),
    (S::PendingCancel, K::Accepted, S::Accepted),
    (S::PendingCancel, K::Updated, S::PendingCancel),
    (S::PendingCancel, K::Filled, S::Filled),
    (S::PendingCancel, K::FillVoided, S::PendingCancel),
    (S::Triggered, K::Rejected, S::Rejected),
    (S::Triggered, K::PendingUpdate, S::PendingUpdate),
    (S::Triggered, K::PendingCancel, S::PendingCancel),
    (S::Triggered, K::Canceled, S::Canceled),
    (S::Triggered, K::Expired, S::Expired),
    (S::Triggered, K::Filled, S::Filled),
    (S::Triggered, K::Updated, S::Triggered),
    (S::Triggered, K::FillVoided, S::Triggered),
    (S::PartiallyFilled, K::PendingUpdate, S::PendingUpdate),
    (S::PartiallyFilled, K::PendingCancel, S::PendingCancel),
    (S::PartiallyFilled, K::Canceled, S::Canceled),
    (S::PartiallyFilled, K::Expired, S::Expired),
    (S::PartiallyFilled, K::Filled, S::Filled),
    (S::PartiallyFilled, K::Accepted, S::Accepted),
    (S::PartiallyFilled, K::Updated, S::PartiallyFilled),
    (S::PartiallyFilled, K::FillVoided, S::PartiallyFilled),
    (S::Filled, K::FillVoided, S::Voided),
    (S::Filled, K::Updated, S::Filled),
    (S::Expired, K::FillVoided, S::Expired),
    (S::Expired, K::Updated, S::Expired),
    (S::Voided, K::FillVoided, S::Voided),
];

/// The status the table moves `from` to on `kind`, or `None` when the table has no such edge.
#[must_use]
pub fn next_status(from: OrderStatus, kind: OrderEventKind) -> Option<OrderStatus> {
    TRANSITIONS.iter().find(|(f, k, _)| *f == from && *k == kind).map(|&(_, _, to)| to)
}

/// Whether the order is in a terminal state. Closed orders never reopen: the table has no edge
/// from a closed status to an open one (`FILLED --FILL_VOIDED--> VOIDED` stays closed).
#[must_use]
pub const fn is_closed(status: OrderStatus) -> bool {
    matches!(
        status,
        OrderStatus::Denied
            | OrderStatus::Rejected
            | OrderStatus::Canceled
            | OrderStatus::Expired
            | OrderStatus::Filled
            | OrderStatus::Voided
    )
}

/// Orders that may still fill or be worked at the venue: everything after submission that is
/// not closed.
#[must_use]
pub const fn is_open(status: OrderStatus) -> bool {
    matches!(
        status,
        OrderStatus::Submitted
            | OrderStatus::Accepted
            | OrderStatus::Triggered
            | OrderStatus::PendingUpdate
            | OrderStatus::PendingCancel
            | OrderStatus::PartiallyFilled
    )
}

#[must_use]
pub const fn is_pending(status: OrderStatus) -> bool {
    matches!(status, OrderStatus::PendingUpdate | OrderStatus::PendingCancel)
}
