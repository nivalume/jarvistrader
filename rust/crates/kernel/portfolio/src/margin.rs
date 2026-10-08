//! Margin models (docs/architecture.md section 11.2): the initial margin a position or an order
//! ties up and the maintenance margin below which the venue liquidates. Both round up onto the
//! currency's grid, so the kernel never assumes more free margin than the venue grants.

use cost::fraction_of;
use kernel_core::{Result, Status};
use model::instruments::Instrument;
use model::{Money, FIXED_SCALAR};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MarginModel {
    /// Fixed fractions from the instrument definition (nautilus `StandardMarginModel`):
    /// `initial = notional x margin_init`, `maintenance = notional x margin_maint`.
    Standard,
    /// Binance USDⓈ-M: `initial = notional / leverage`, `maintenance = notional x margin_maint`.
    Leveraged { leverage: u32 },
}

impl MarginModel {
    pub fn leveraged(leverage: u32) -> Result<Self> {
        if leverage == 0 || leverage > 1000 {
            return Err(Status::InvalidArgument);
        }
        Ok(MarginModel::Leveraged { leverage })
    }

    /// The initial margin fraction (10^9 scale), rounded up for a leverage that does not divide.
    #[must_use]
    pub fn initial_fraction(&self, instrument: &Instrument) -> u64 {
        match self {
            MarginModel::Standard => instrument.margin_init.raw(),
            MarginModel::Leveraged { leverage } => {
                (FIXED_SCALAR as u64).div_ceil(u64::from(*leverage))
            }
        }
    }

    /// Initial margin of a notional (10^9 raw magnitude) in the settlement currency.
    pub fn initial(&self, instrument: &Instrument, notional_raw: u64) -> Result<Money> {
        let currency = instrument.settlement_currency();
        let raw = fraction_of(notional_raw, self.initial_fraction(instrument), currency, true)?;
        Money::from_raw_exact(i64::try_from(raw).map_err(|_| Status::Overflow)?, currency)
    }

    /// Maintenance margin of a notional.
    pub fn maintenance(&self, instrument: &Instrument, notional_raw: u64) -> Result<Money> {
        let currency = instrument.settlement_currency();
        let raw = fraction_of(notional_raw, instrument.margin_maint.raw(), currency, true)?;
        Money::from_raw_exact(i64::try_from(raw).map_err(|_| Status::Overflow)?, currency)
    }
}
