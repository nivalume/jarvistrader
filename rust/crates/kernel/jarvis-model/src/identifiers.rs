//! Identifiers and their string constraints (docs/architecture.md section 6.2).

use core::fmt;
use core::str::FromStr;

use jarvis_core::{FixedString, Result, Status};

fn ascii_non_blank(s: &str) -> bool {
    !s.is_empty() && s.is_ascii() && !s.bytes().all(|b| b == b' ')
}
fn utf8_non_blank(s: &str) -> bool {
    !s.is_empty() && !s.chars().all(char::is_whitespace)
}

/// Declares a newtype over a `FixedString`, validated by `$check`, with the string traits.
macro_rules! identifier {
    ($(#[$doc:meta])* $name:ident, $cap:literal, $check:expr) => {
        $(#[$doc])*
        #[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
        pub struct $name(FixedString<$cap>);

        impl $name {
            pub const CAPACITY: usize = $cap;

            /// `InvalidArgument` when the text breaks the identifier's rule, `OutOfRange` when it
            /// is longer than the capacity.
            pub fn new(text: &str) -> Result<Self> {
                let check: fn(&str) -> bool = $check;
                if !check(text) {
                    return Err(Status::InvalidArgument);
                }
                Ok(Self(FixedString::from_text(text)?))
            }
            #[must_use]
            pub fn as_str(&self) -> &str {
                self.0.as_str()
            }
            #[must_use]
            pub fn inner(&self) -> &FixedString<$cap> {
                &self.0
            }
            /// Wraps an already validated string (a decoder re-checks the rule).
            pub fn from_fixed(s: FixedString<$cap>) -> Result<Self> {
                Self::new(s.as_str())
            }
        }
        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(self.as_str())
            }
        }
        impl fmt::Debug for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}({:?})", stringify!($name), self.as_str())
            }
        }
        impl FromStr for $name {
            type Err = Status;
            fn from_str(s: &str) -> Result<Self> {
                Self::new(s)
            }
        }
    };
}

identifier!(
    /// Non-empty, not all blank, UTF-8; may contain `.`.
    Symbol, 32, utf8_non_blank
);
identifier!(
    /// Non-empty, not all blank, ASCII, no `.`.
    Venue, 16, |s| ascii_non_blank(s) && !s.contains('.')
);
identifier!(
    /// ASCII, must contain `-`; the tag is after the **last** `-`. `EXTERNAL-0` is reserved.
    TraderId, 32, |s| ascii_non_blank(s) && s.rfind('-').is_some_and(|i| i > 0 && i + 1 < s.len())
);
identifier!(
    /// As [`TraderId`], or the literal `EXTERNAL`.
    StrategyId, 32, |s| s == "EXTERNAL" || (ascii_non_blank(s) && s.rfind('-').is_some_and(|i| i > 0 && i + 1 < s.len()))
);
identifier!(
    /// ASCII, must contain `-`; issuer and number split at the **first** `-`.
    AccountId, 32, |s| ascii_non_blank(s) && s.find('-').is_some_and(|i| i > 0 && i + 1 < s.len())
);
identifier!(
    /// ASCII; `EXTERNAL` is reserved for orders the node did not place. jarvis's own format is in
    /// `client_order_id`.
    ClientOrderId, 36, ascii_non_blank
);
identifier!(ClientId, 32, ascii_non_blank);
identifier!(ComponentId, 32, ascii_non_blank);
identifier!(ExecAlgorithmId, 32, ascii_non_blank);
identifier!(OrderListId, 32, ascii_non_blank);
identifier!(VenueOrderId, 36, ascii_non_blank);
identifier!(
    /// UTF-8; under NETTING it is `{instrument_id}-{strategy_id}`.
    PositionId, 96, utf8_non_blank
);
identifier!(
    /// Non-empty ASCII, at most 36 characters.
    TradeId, 36, ascii_non_blank
);

impl TraderId {
    /// The text after the last `-`.
    #[must_use]
    pub fn tag(&self) -> &str {
        self.as_str().rsplit_once('-').map_or("", |(_, tag)| tag)
    }
}
impl StrategyId {
    #[must_use]
    pub fn tag(&self) -> &str {
        self.as_str().rsplit_once('-').map_or("", |(_, tag)| tag)
    }
    #[must_use]
    pub fn is_external(&self) -> bool {
        self.as_str() == "EXTERNAL"
    }
}
impl AccountId {
    #[must_use]
    pub fn issuer(&self) -> &str {
        self.as_str().split_once('-').map_or("", |(issuer, _)| issuer)
    }
    #[must_use]
    pub fn number(&self) -> &str {
        self.as_str().split_once('-').map_or("", |(_, number)| number)
    }
}
impl ClientOrderId {
    #[must_use]
    pub fn is_external(&self) -> bool {
        self.as_str() == "EXTERNAL"
    }
}

/// `"{symbol}.{venue}"`, split at the **last** `.`.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct InstrumentId {
    pub symbol: Symbol,
    pub venue: Venue,
}

impl InstrumentId {
    #[must_use]
    pub const fn new(symbol: Symbol, venue: Venue) -> Self {
        Self { symbol, venue }
    }

    pub fn parse(text: &str) -> Result<Self> {
        let (symbol, venue) = text.rsplit_once('.').ok_or(Status::ParseError)?;
        Ok(Self { symbol: Symbol::new(symbol)?, venue: Venue::new(venue)? })
    }

    /// Writes `"{symbol}.{venue}"` into `out`; returns the length. `out` needs
    /// `Symbol::CAPACITY + 1 + Venue::CAPACITY` bytes.
    pub fn write_to(&self, out: &mut [u8]) -> usize {
        let s = self.symbol.as_str().as_bytes();
        let v = self.venue.as_str().as_bytes();
        out[..s.len()].copy_from_slice(s);
        out[s.len()] = b'.';
        out[s.len() + 1..s.len() + 1 + v.len()].copy_from_slice(v);
        s.len() + 1 + v.len()
    }
}
impl fmt::Display for InstrumentId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}", self.symbol, self.venue)
    }
}
impl fmt::Debug for InstrumentId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "InstrumentId({self})")
    }
}
impl FromStr for InstrumentId {
    type Err = Status;
    fn from_str(s: &str) -> Result<Self> {
        Self::parse(s)
    }
}

impl PositionId {
    /// The NETTING position id `{instrument_id}-{strategy_id}`.
    pub fn netting(instrument_id: &InstrumentId, strategy_id: &StrategyId) -> Result<Self> {
        let mut buf = [0u8; Symbol::CAPACITY + 1 + Venue::CAPACITY + 1 + StrategyId::CAPACITY];
        let n = instrument_id.write_to(&mut buf);
        buf[n] = b'-';
        let s = strategy_id.as_str().as_bytes();
        buf[n + 1..n + 1 + s.len()].copy_from_slice(s);
        let text =
            core::str::from_utf8(&buf[..n + 1 + s.len()]).map_err(|_| Status::InvalidArgument)?;
        Self::new(text)
    }
}
