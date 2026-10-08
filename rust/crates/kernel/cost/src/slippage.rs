//! Expected cost of taking liquidity. v1.0 has one static model: walk the opposite side of the
//! book, best level first, until the quantity is filled. Levels are any type with a price and a
//! size, best first, so the cost layer does not depend on the book implementation.

use kernel_core::{Result, Status};
use model::enums::OrderSide;
use model::{Price, Quantity, FIXED_PRECISION, FIXED_SCALAR};

/// A price level as the slippage model sees it.
pub trait Level {
    fn price(&self) -> Price;
    fn size(&self) -> Quantity;
}

impl Level for (Price, Quantity) {
    fn price(&self) -> Price {
        self.0
    }
    fn size(&self) -> Quantity {
        self.1
    }
}

/// What taking `quantity` would cost.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SlippageEstimate {
    /// How much the book could fill; less than asked when the levels ran out.
    pub filled: Quantity,
    /// Average fill price, rounded against the taker onto the full 10^9 grid.
    pub average_price: Price,
    /// The deepest level touched.
    pub worst_price: Price,
    /// `|average - best|` on the 10^9 scale: the slippage per unit versus the touch.
    pub slippage_per_unit_raw: u64,
    /// Levels consumed, the last one possibly in part.
    pub levels: usize,
}

/// Walks the book.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BookDepthSlippage;

impl BookDepthSlippage {
    /// `side` is the taker's side: a BUY walks the asks (`levels` are the asks, best first).
    /// `InvalidArgument` without levels or for a zero quantity.
    pub fn estimate<L: Level>(
        side: OrderSide,
        quantity: Quantity,
        levels: &[L],
    ) -> Result<SlippageEstimate> {
        let first = levels.first().ok_or(Status::InvalidArgument)?;
        if quantity.is_undef() || quantity.is_zero() {
            return Err(Status::InvalidArgument);
        }
        let best = first.price();
        let mut remaining = quantity.raw();
        let mut notional: i128 = 0; // sum of price.raw x size.raw, 10^18 scale
        let mut filled: u64 = 0;
        let mut worst = best;
        let mut used = 0;
        for level in levels {
            if remaining == 0 {
                break;
            }
            let take = remaining.min(level.size().raw());
            if take == 0 {
                continue;
            }
            notional += i128::from(level.price().raw()) * i128::from(take);
            filled += take;
            remaining -= take;
            worst = level.price();
            used += 1;
        }
        if filled == 0 {
            return Err(Status::InvalidArgument);
        }
        // Average against the taker: a buyer's average rounds up, a seller's down.
        let filled_i = i128::from(filled);
        let avg = match side {
            OrderSide::Buy => (notional + filled_i - 1).div_euclid(filled_i),
            OrderSide::Sell => notional.div_euclid(filled_i),
        };
        let avg = i64::try_from(avg).map_err(|_| Status::Overflow)?;
        let average_price = Price::from_raw(avg, FIXED_PRECISION)?;
        let slip = match side {
            OrderSide::Buy => avg - best.raw(),
            OrderSide::Sell => best.raw() - avg,
        };
        Ok(SlippageEstimate {
            filled: Quantity::from_raw(filled, quantity.precision().max(first.size().precision()))?,
            average_price,
            worst_price: worst,
            slippage_per_unit_raw: slip.max(0).unsigned_abs(),
            levels: used,
        })
    }

    /// The slippage cost of an estimate in quote units on the 10^9 scale:
    /// `slippage_per_unit x filled x multiplier`.
    pub fn cost_raw(estimate: &SlippageEstimate, multiplier: Quantity) -> Result<i64> {
        let per_unit = Price::from_raw(
            i64::try_from(estimate.slippage_per_unit_raw).map_err(|_| Status::Overflow)?,
            FIXED_PRECISION,
        )?;
        model::fixed_point::notional_raw(per_unit, estimate.filled, multiplier)
    }
}

/// A multiplier of one contract per unit, for callers without an instrument.
#[must_use]
pub fn unit_multiplier() -> Quantity {
    Quantity::from_raw(FIXED_SCALAR as u64, 0).unwrap_or_default()
}
