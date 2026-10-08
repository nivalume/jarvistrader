//! Account state (docs/architecture.md section 6.7).

use jarvis_core::{FixedVec, Result, Status, UnixNanos};

use crate::currency::Currency;
use crate::enums::AccountType;
use crate::identifiers::{AccountId, InstrumentId};
use crate::money::Money;
use crate::uuid::Uuid4;
use crate::wire::{Wire, WireReader, WireWriter};

/// `total == locked + free`, all in one currency.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct AccountBalance {
    pub currency: Currency,
    pub total: Money,
    pub locked: Money,
    pub free: Money,
}

impl AccountBalance {
    pub fn new(total: Money, locked: Money, free: Money) -> Result<Self> {
        let currency = total.currency();
        if locked.currency() != currency || free.currency() != currency {
            return Err(Status::InvalidArgument);
        }
        if locked.is_negative() || free.is_negative() {
            return Err(Status::InvalidArgument);
        }
        if locked.checked_add(free)? != total {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { currency, total, locked, free })
    }
}
impl Wire for AccountBalance {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.total);
        w.put(&self.locked);
        w.put(&self.free);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MarginBalance {
    pub initial: Money,
    pub maintenance: Money,
    pub currency: Currency,
    pub instrument_id: Option<InstrumentId>,
}

impl MarginBalance {
    pub fn new(
        initial: Money,
        maintenance: Money,
        instrument_id: Option<InstrumentId>,
    ) -> Result<Self> {
        if initial.currency() != maintenance.currency()
            || initial.is_negative()
            || maintenance.is_negative()
        {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { initial, maintenance, currency: initial.currency(), instrument_id })
    }
}
impl Wire for MarginBalance {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.initial);
        w.put(&self.maintenance);
        w.put(&self.instrument_id);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(r.get()?, r.get()?, r.get()?)
    }
}

/// A venue's report of the account. `info` (a free dictionary in nautilus) is omitted.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountState {
    pub account_id: AccountId,
    pub account_type: AccountType,
    pub base_currency: Option<Currency>,
    pub balances: FixedVec<AccountBalance>,
    pub margins: FixedVec<MarginBalance>,
    pub is_reported: bool,
    pub event_id: Uuid4,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl AccountState {
    /// At least one balance, one per currency, `ts_init >= ts_event`.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        account_id: AccountId,
        account_type: AccountType,
        base_currency: Option<Currency>,
        balances: FixedVec<AccountBalance>,
        margins: FixedVec<MarginBalance>,
        is_reported: bool,
        event_id: Uuid4,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if balances.is_empty() || ts_init < ts_event {
            return Err(Status::InvalidArgument);
        }
        for (i, a) in balances.iter().enumerate() {
            if balances[..i].iter().any(|b| b.currency == a.currency) {
                return Err(Status::AlreadyExists);
            }
        }
        Ok(Self {
            account_id,
            account_type,
            base_currency,
            balances,
            margins,
            is_reported,
            event_id,
            ts_event,
            ts_init,
        })
    }

    #[must_use]
    pub fn balance(&self, currency: Currency) -> Option<&AccountBalance> {
        self.balances.iter().find(|b| b.currency == currency)
    }
}
impl Wire for AccountState {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.account_id);
        w.put(&self.account_type);
        w.put(&self.base_currency);
        w.put(&self.balances);
        w.put(&self.margins);
        w.put(&self.is_reported);
        w.put(&self.event_id);
        w.put(&self.ts_event);
        w.put(&self.ts_init);
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        Self::new(
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
        )
    }
}
