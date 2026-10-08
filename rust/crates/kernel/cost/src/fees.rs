//! Trading fees and funding. Both are computed exactly in integers from the fill's notional and
//! rounded onto the currency's grid against the account: fees and funding paid round up, rebates
//! and funding received round down. A backtest therefore never books a cost smaller than the venue
//! would charge.

use kernel_core::int_math::{mul_div_u64, mul_div_u64_up, POW10};
use kernel_core::{Result, Status};
use model::enums::LiquiditySide;
use model::instruments::Instrument;
use model::{Currency, Money, Price, Quantity, FIXED_PRECISION, FIXED_SCALAR};

/// A rate as a fraction on the 10^9 scale: `Rate::bps(2)` is 0.02%.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Rate(pub i64);

impl Rate {
    pub const ONE: Rate = Rate(FIXED_SCALAR);

    #[must_use]
    pub const fn bps(bps: i64) -> Rate {
        Rate(bps * FIXED_SCALAR / 10_000)
    }
    /// Parses a decimal fraction such as `"0.0002"`; at most nine digits.
    pub fn parse(text: &str) -> Result<Rate> {
        let d = model::decimal::parse(text)?;
        let raw = model::decimal::to_raw(&d, FIXED_PRECISION)?;
        i64::try_from(raw).map(Rate).map_err(|_| Status::Overflow)
    }
    #[must_use]
    pub const fn is_negative(self) -> bool {
        self.0 < 0
    }
}

/// `|amount| x |fraction|`, both on the 10^9 scale, as a magnitude on the grid of `currency`'s
/// precision, rounded `up` or down. Exact through 192 bits.
pub fn fraction_of(
    amount_raw: u64,
    fraction_raw: u64,
    currency: Currency,
    up: bool,
) -> Result<u64> {
    let grid = POW10[(FIXED_PRECISION - currency.precision) as usize];
    // amount x fraction / 1e9, then onto the grid: divide by (1e9 x grid), multiply back by grid.
    let divisor = (FIXED_SCALAR as u64).checked_mul(grid).ok_or(Status::Overflow)?;
    let units = if up {
        mul_div_u64_up(amount_raw, fraction_raw, 1, divisor)?
    } else {
        mul_div_u64(amount_raw, fraction_raw, 1, divisor)?
    };
    units.checked_mul(grid).ok_or(Status::Overflow)
}

/// A signed amount of `currency`, rounded against the account: what the account pays rounds up in
/// magnitude, what it receives rounds down. `negative` sets the sign of the result.
fn against_account(
    magnitude_raw: u64,
    rate_raw: u64,
    currency: Currency,
    account_pays: bool,
    negative: bool,
) -> Result<Money> {
    let m = fraction_of(magnitude_raw, rate_raw, currency, account_pays)?;
    let raw = i64::try_from(m).map_err(|_| Status::Overflow)?;
    Money::from_raw_exact(if negative { -raw } else { raw }, currency)
}

/// Maker and taker rates (fractions of notional) and a discount on positive fees (Binance: 10% on
/// USDⓈ-M futures when paid in BNB). Negative maker rates are rebates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MakerTakerFees {
    maker: Rate,
    taker: Rate,
    /// Fraction of a positive fee waived, on the 10^9 scale.
    discount: Rate,
}

impl MakerTakerFees {
    /// `InvalidArgument` for a negative taker rate or a discount outside `[0, 1]`.
    pub fn new(maker: Rate, taker: Rate, discount: Rate) -> Result<Self> {
        if taker.is_negative() || discount.is_negative() || discount > Rate::ONE {
            return Err(Status::InvalidArgument);
        }
        Ok(Self { maker, taker, discount })
    }

    /// Built-in schedules (Binance standard rates):
    ///
    /// | name | maker | taker | discount |
    /// | --- | --- | --- | --- |
    /// | `binance_usdm_vip0` | 0.0200% | 0.0500% | none |
    /// | `binance_usdm_vip0_bnb` | 0.0200% | 0.0500% | 10% |
    /// | `binance_spot_vip0` | 0.1000% | 0.1000% | none |
    /// | `binance_spot_vip0_bnb` | 0.1000% | 0.1000% | 25% |
    /// | `zero` | 0 | 0 | none |
    pub fn schedule(name: &str) -> Result<Self> {
        let pct = |p: i64| Rate(p); // p is in 10^-9 units of 1.0
        let (maker, taker, discount) = match name {
            "binance_usdm_vip0" => (pct(200_000), pct(500_000), Rate(0)),
            "binance_usdm_vip0_bnb" => (pct(200_000), pct(500_000), Rate(100_000_000)),
            "binance_spot_vip0" => (pct(1_000_000), pct(1_000_000), Rate(0)),
            "binance_spot_vip0_bnb" => (pct(1_000_000), pct(1_000_000), Rate(250_000_000)),
            "zero" => (Rate(0), Rate(0), Rate(0)),
            _ => return Err(Status::NotFound),
        };
        Self::new(maker, taker, discount)
    }

    #[must_use]
    pub const fn maker(&self) -> Rate {
        self.maker
    }
    #[must_use]
    pub const fn taker(&self) -> Rate {
        self.taker
    }
    #[must_use]
    pub const fn discount(&self) -> Rate {
        self.discount
    }

    /// The commission of a fill of `quantity` at `price`: `notional x rate x (1 - discount)` in
    /// the quote currency, with the sign venues report (and `OrderFilled::commission` carries):
    /// positive = paid by the account, negative = a rebate. A fill without a liquidity side pays
    /// the taker rate.
    pub fn commission(
        &self,
        instrument: &Instrument,
        price: Price,
        quantity: Quantity,
        side: LiquiditySide,
    ) -> Result<Money> {
        let notional = model::fixed_point::notional_raw(price, quantity, instrument.multiplier)?;
        let notional = notional.unsigned_abs();
        let currency = instrument.quote_currency;
        let rate = match side {
            LiquiditySide::Maker => self.maker,
            LiquiditySide::Taker | LiquiditySide::NoLiquiditySide => self.taker,
        };
        if rate.is_negative() {
            // A rebate: received, rounded down; no discount applies.
            return against_account(notional, rate.0.unsigned_abs(), currency, false, true);
        }
        // Fee x (1 - discount), both on the 10^9 scale, combined exactly before rounding once.
        let effective = mul_div_u64(
            rate.0 as u64,
            (FIXED_SCALAR - self.discount.0) as u64,
            1,
            FIXED_SCALAR as u64,
        )?;
        against_account(notional, effective, currency, true, false)
    }
}

/// Funding exchanged by a position at a settlement (Binance USDⓈ-M perpetuals): the position's
/// notional at the mark price times the rate; longs pay a positive rate to shorts. `signed_qty_raw`
/// is the position (positive long), `rate` the funding rate (10^9 scale). The result is the
/// account's cash flow: positive = received, negative = paid.
pub fn funding(
    instrument: &Instrument,
    signed_qty_raw: i64,
    mark: Price,
    rate: Rate,
) -> Result<Money> {
    if signed_qty_raw == 0 || rate.0 == 0 {
        return Ok(Money::zero(instrument.settlement_currency()));
    }
    let quantity = Quantity::from_raw(signed_qty_raw.unsigned_abs(), FIXED_PRECISION)?;
    let notional =
        model::fixed_point::notional_raw(mark, quantity, instrument.multiplier)?.unsigned_abs();
    // A long pays a positive rate; a short pays a negative rate.
    let pays = (signed_qty_raw > 0) == (rate.0 > 0);
    against_account(notional, rate.0.unsigned_abs(), instrument.settlement_currency(), pays, pays)
}
