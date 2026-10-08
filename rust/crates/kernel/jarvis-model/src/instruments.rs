//! Instruments (docs/architecture.md section 6.5): the fields every instrument has, plus the kind
//! with its own. `tick_scheme` and `info` are deliberately absent (section 6.8).

use jarvis_core::{Result, Status, UnixNanos};

use crate::currency::Currency;
use crate::enums::{AssetClass, InstrumentClass};
use crate::fixed_point::{Price, Quantity, FIXED_PRECISION};
use crate::identifiers::{InstrumentId, Symbol};
use crate::wire::{Wire, WireReader, WireWriter};

/// What distinguishes the instrument kinds jarvis trades.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum InstrumentKind {
    /// Spot, for example `BTCUSDT.BINANCE`.
    CurrencyPair,
    /// A perpetual swap, for example `BTCUSDT-PERP.BINANCE`.
    CryptoPerpetual { settlement_currency: Currency, is_inverse: bool },
    /// A dated future; the symbol keeps the `_YYMMDD` suffix.
    CryptoFuture {
        underlying: Currency,
        settlement_currency: Currency,
        is_inverse: bool,
        activation_ns: UnixNanos,
        expiration_ns: UnixNanos,
    },
}

impl InstrumentKind {
    #[must_use]
    pub const fn instrument_class(&self) -> InstrumentClass {
        match self {
            InstrumentKind::CurrencyPair => InstrumentClass::Spot,
            InstrumentKind::CryptoPerpetual { .. } => InstrumentClass::Swap,
            InstrumentKind::CryptoFuture { .. } => InstrumentClass::Future,
        }
    }
}

impl Wire for InstrumentKind {
    fn encode(&self, w: &mut WireWriter) {
        match self {
            InstrumentKind::CurrencyPair => w.u8(1),
            InstrumentKind::CryptoPerpetual { settlement_currency, is_inverse } => {
                w.u8(2);
                w.put(settlement_currency);
                w.put(is_inverse);
            }
            InstrumentKind::CryptoFuture {
                underlying,
                settlement_currency,
                is_inverse,
                activation_ns,
                expiration_ns,
            } => {
                w.u8(3);
                w.put(underlying);
                w.put(settlement_currency);
                w.put(is_inverse);
                w.put(activation_ns);
                w.put(expiration_ns);
            }
        }
    }
    fn decode(r: &mut WireReader<'_>) -> Result<Self> {
        match r.u8()? {
            1 => Ok(InstrumentKind::CurrencyPair),
            2 => Ok(InstrumentKind::CryptoPerpetual {
                settlement_currency: r.get()?,
                is_inverse: r.get()?,
            }),
            3 => {
                let underlying = r.get()?;
                let settlement_currency = r.get()?;
                let is_inverse = r.get()?;
                let activation_ns: UnixNanos = r.get()?;
                let expiration_ns: UnixNanos = r.get()?;
                if expiration_ns <= activation_ns {
                    return Err(Status::InvalidArgument);
                }
                Ok(InstrumentKind::CryptoFuture {
                    underlying,
                    settlement_currency,
                    is_inverse,
                    activation_ns,
                    expiration_ns,
                })
            }
            _ => Err(Status::InvalidArgument),
        }
    }
}

/// The limits a venue applies to orders in this instrument. `None` means the venue sets none.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct InstrumentLimits {
    pub max_quantity: Option<Quantity>,
    pub min_quantity: Option<Quantity>,
    pub max_notional: Option<crate::money::Money>,
    pub min_notional: Option<crate::money::Money>,
    pub max_price: Option<Price>,
    pub min_price: Option<Price>,
}
crate::wire_struct!(InstrumentLimits {
    max_quantity,
    min_quantity,
    max_notional,
    min_notional,
    max_price,
    min_price
});

/// An instrument definition. Margin rates are fixed-point on the 1e9 scale (`Quantity`, so they
/// cannot be negative; `1.0` is `FIXED_SCALAR`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Instrument {
    pub id: InstrumentId,
    pub raw_symbol: Symbol,
    pub asset_class: AssetClass,
    pub kind: InstrumentKind,
    pub base_currency: Option<Currency>,
    pub quote_currency: Currency,
    pub price_precision: u8,
    pub size_precision: u8,
    pub price_increment: Price,
    pub size_increment: Quantity,
    pub multiplier: Quantity,
    pub lot_size: Option<Quantity>,
    pub limits: InstrumentLimits,
    pub margin_init: Quantity,
    pub margin_maint: Quantity,
    pub ts_event: UnixNanos,
    pub ts_init: UnixNanos,
}

impl Instrument {
    /// Validates (section 6.5): increments and multiplier positive, the increments' precisions
    /// equal to the declared ones, precisions at most nine, `min <= max` for every limit pair, and
    /// a future expires after it activates.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        id: InstrumentId,
        raw_symbol: Symbol,
        asset_class: AssetClass,
        kind: InstrumentKind,
        base_currency: Option<Currency>,
        quote_currency: Currency,
        price_increment: Price,
        size_increment: Quantity,
        multiplier: Quantity,
        lot_size: Option<Quantity>,
        limits: InstrumentLimits,
        margin_init: Quantity,
        margin_maint: Quantity,
        ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> Result<Self> {
        if !price_increment.is_positive() || size_increment.is_zero() || multiplier.is_zero() {
            return Err(Status::InvalidArgument);
        }
        if price_increment.is_undef()
            || size_increment.is_undef()
            || multiplier.is_undef()
            || margin_init.is_undef()
            || margin_maint.is_undef()
        {
            return Err(Status::InvalidArgument);
        }
        let price_precision = price_increment.precision();
        let size_precision = size_increment.precision();
        if price_precision > FIXED_PRECISION || size_precision > FIXED_PRECISION {
            return Err(Status::PrecisionLoss);
        }
        if let Some(lot) = lot_size {
            if lot.is_zero() || lot.is_undef() {
                return Err(Status::InvalidArgument);
            }
        }
        if let (Some(lo), Some(hi)) = (limits.min_quantity, limits.max_quantity) {
            if lo > hi {
                return Err(Status::InvalidArgument);
            }
        }
        if let (Some(lo), Some(hi)) = (limits.min_price, limits.max_price) {
            if lo > hi {
                return Err(Status::InvalidArgument);
            }
        }
        if let (Some(lo), Some(hi)) = (limits.min_notional, limits.max_notional) {
            if lo.currency() != hi.currency() || lo.raw() > hi.raw() {
                return Err(Status::InvalidArgument);
            }
        }
        if let InstrumentKind::CryptoFuture { activation_ns, expiration_ns, .. } = kind {
            if expiration_ns <= activation_ns {
                return Err(Status::InvalidArgument);
            }
        }
        if ts_init < ts_event {
            return Err(Status::InvalidArgument);
        }
        Ok(Self {
            id,
            raw_symbol,
            asset_class,
            kind,
            base_currency,
            quote_currency,
            price_precision,
            size_precision,
            price_increment,
            size_increment,
            multiplier,
            lot_size,
            limits,
            margin_init,
            margin_maint,
            ts_event,
            ts_init,
        })
    }

    #[must_use]
    pub const fn instrument_class(&self) -> InstrumentClass {
        self.kind.instrument_class()
    }

    /// The currency positions settle in: the kind's settlement currency, else the quote.
    #[must_use]
    pub const fn settlement_currency(&self) -> Currency {
        match self.kind {
            InstrumentKind::CurrencyPair => self.quote_currency,
            InstrumentKind::CryptoPerpetual { settlement_currency, .. }
            | InstrumentKind::CryptoFuture { settlement_currency, .. } => settlement_currency,
        }
    }

    #[must_use]
    pub const fn is_inverse(&self) -> bool {
        match self.kind {
            InstrumentKind::CurrencyPair => false,
            InstrumentKind::CryptoPerpetual { is_inverse, .. }
            | InstrumentKind::CryptoFuture { is_inverse, .. } => is_inverse,
        }
    }

    /// Whether `price` is on the instrument's price grid.
    #[must_use]
    pub fn is_valid_price(&self, price: Price) -> bool {
        !price.is_undef() && price.raw() % self.price_increment.raw() == 0
    }
    /// Whether `size` is on the instrument's size grid and positive.
    #[must_use]
    pub fn is_valid_size(&self, size: Quantity) -> bool {
        !size.is_undef() && !size.is_zero() && size.raw() % self.size_increment.raw() == 0
    }
}

impl Wire for Instrument {
    fn encode(&self, w: &mut WireWriter) {
        w.put(&self.id);
        w.put(&self.raw_symbol);
        w.put(&self.asset_class);
        w.put(&self.kind);
        w.put(&self.base_currency);
        w.put(&self.quote_currency);
        w.put(&self.price_increment);
        w.put(&self.size_increment);
        w.put(&self.multiplier);
        w.put(&self.lot_size);
        w.put(&self.limits);
        w.put(&self.margin_init);
        w.put(&self.margin_maint);
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
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
            r.get()?,
        )
    }
}
