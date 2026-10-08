//! Currencies: the nautilus built-in table plus any the node registers from `exchangeInfo`.

use core::fmt;
use core::str::FromStr;

use jarvis_core::{FixedString, Result, Status};

pub use crate::enums::CurrencyType;
use crate::generated::currencies::BUILTIN;

pub type CurrencyCode = FixedString<8>;
pub type CurrencyName = FixedString<32>;

/// `{ code, precision, iso4217, name, currency_type }`. Equality and order are by code: the table
/// has one entry per code, and a currency registered twice with different fields is a defect
/// the registration refuses.
#[derive(Clone, Copy)]
pub struct Currency {
    pub code: CurrencyCode,
    pub precision: u8,
    pub iso4217: u16,
    pub name: CurrencyName,
    pub currency_type: CurrencyType,
}

impl Currency {
    /// A table entry; only the generated table calls it, with values that are known to fit.
    pub(crate) const fn builtin(
        code: &str,
        precision: u8,
        iso4217: u16,
        name: &str,
        currency_type: CurrencyType,
    ) -> Self {
        Self {
            code: FixedString::from_static(code),
            precision,
            iso4217,
            name: FixedString::from_static(name),
            currency_type,
        }
    }

    /// A currency not in the built-in table (a new listing). `code` is 1 to 8 upper-case ASCII
    /// letters or digits; `precision` at most nine.
    pub fn new(
        code: &str,
        precision: u8,
        iso4217: u16,
        name: &str,
        currency_type: CurrencyType,
    ) -> Result<Self> {
        if code.is_empty() || !code.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit()) {
            return Err(Status::InvalidArgument);
        }
        if precision > crate::fixed_point::FIXED_PRECISION {
            return Err(Status::InvalidArgument);
        }
        Ok(Self {
            code: FixedString::from_text(code).map_err(|_| Status::InvalidArgument)?,
            precision,
            iso4217,
            name: FixedString::from_text(name).map_err(|_| Status::InvalidArgument)?,
            currency_type,
        })
    }

    /// The built-in currency with this code, if nautilus defines one.
    #[must_use]
    pub fn builtin_by_code(code: &str) -> Option<Currency> {
        BUILTIN.iter().copied().find(|c| c.code.as_str() == code)
    }

    /// Every built-in currency, in nautilus's order.
    #[must_use]
    pub fn builtins() -> &'static [Currency] {
        BUILTIN
    }

    #[must_use]
    pub fn code(&self) -> &str {
        self.code.as_str()
    }
}

impl PartialEq for Currency {
    fn eq(&self, other: &Self) -> bool {
        self.code == other.code
    }
}
impl Eq for Currency {}
impl PartialOrd for Currency {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Currency {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.code.cmp(&other.code)
    }
}
impl core::hash::Hash for Currency {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.code.hash(state);
    }
}
impl Default for Currency {
    /// `USDT`: the settlement currency of every v1.0 instrument.
    fn default() -> Self {
        Self::builtin_by_code("USDT")
            .unwrap_or_else(|| Self::builtin("USDT", 8, 0, "Tether", CurrencyType::Crypto))
    }
}
impl fmt::Debug for Currency {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Currency({})", self.code)
    }
}
impl fmt::Display for Currency {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.code.as_str())
    }
}
impl FromStr for Currency {
    type Err = Status;
    /// A built-in currency by code.
    fn from_str(s: &str) -> Result<Self> {
        Self::builtin_by_code(s).ok_or(Status::NotFound)
    }
}
