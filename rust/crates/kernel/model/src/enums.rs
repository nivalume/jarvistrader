//! Model enums with stable integer values and `SCREAMING_SNAKE_CASE` strings
//! (docs/architecture.md section 6.6). Zero-valued variants are preserved for
//! `AggressorSide`, `LiquiditySide`, `OmsType`, and `MarketStatusAction`; other absence is `Option`.
//!
//! Parsing is case-insensitive and accepts explicitly declared aliases.

use core::fmt;
use core::str::FromStr;

use kernel_core::{Result, Status};

/// Declares a Jarvis enum with stable values, strings, aliases, and conversions.
macro_rules! jarvis_enum {
    ($(#[$doc:meta])* $name:ident : $repr:ty { $($variant:ident = $value:literal => $string:literal $([$($alias:literal),*])?),+ $(,)? }) => {
        $(#[$doc])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        #[repr($repr)]
        pub enum $name {
            $($variant = $value,)+
        }

        impl $name {
            /// Every variant, in declaration order.
            pub const ALL: &'static [$name] = &[$($name::$variant,)+];

            /// The `SCREAMING_SNAKE_CASE` string.
            #[must_use]
            pub const fn as_str(self) -> &'static str {
                match self {
                    $($name::$variant => $string,)+
                }
            }

            #[must_use]
            pub const fn value(self) -> $repr {
                self as $repr
            }

            /// The variant with this integer value.
            pub const fn from_value(value: $repr) -> Result<Self> {
                match value {
                    $($value => Ok(Self::$variant),)+
                    _ => Err(Status::InvalidArgument),
                }
            }

            /// The variant with this string or alias, ignoring case.
            pub fn from_text(text: &str) -> Result<Self> {
                $(
                    if text.eq_ignore_ascii_case($string) { return Ok($name::$variant); }
                    $($(if text.eq_ignore_ascii_case($alias) { return Ok($name::$variant); })*)?
                )+
                Err(Status::ParseError)
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(self.as_str())
            }
        }
        /// The first variant: a placeholder for containers that need a value before reading a
        /// snapshot into it, never a meaningful default.
        impl Default for $name {
            fn default() -> Self {
                Self::ALL[0]
            }
        }
        impl FromStr for $name {
            type Err = Status;
            fn from_str(s: &str) -> Result<Self> {
                Self::from_text(s)
            }
        }
        impl kernel_core::state::State for $name {
            fn write(&self, w: &mut kernel_core::state::StateWriter<'_>) {
                kernel_core::state::State::write(&(*self as $repr), w);
            }
            fn read(&mut self, r: &mut kernel_core::state::StateReader<'_>) {
                let mut raw: $repr = 0;
                kernel_core::state::State::read(&mut raw, r);
                match Self::from_value(raw) {
                    Ok(v) => *self = v,
                    Err(e) => r.fail(e),
                }
            }
        }
        impl crate::wire::Wire for $name {
            fn encode(&self, w: &mut crate::wire::WireWriter) {
                crate::wire::Wire::encode(&(*self as $repr), w);
            }
            fn decode(r: &mut crate::wire::WireReader<'_>) -> Result<Self> {
                Self::from_value(<$repr as crate::wire::Wire>::decode(r)?)
            }
        }
    };
}

jarvis_enum!(AccountType: u8 { Cash = 1 => "CASH", Margin = 2 => "MARGIN", Betting = 3 => "BETTING", Wallet = 4 => "WALLET" });
jarvis_enum!(AggregationSource: u8 { External = 1 => "EXTERNAL", Internal = 2 => "INTERNAL" });
jarvis_enum!(
    /// Who initiated a trade. `NoAggressor` is kept because venues report trades without it.
    AggressorSide: u8 { NoAggressor = 0 => "NO_AGGRESSOR", Buy = 1 => "BUY" ["BUYER"], Sell = 2 => "SELL" ["SELLER"] }
);
jarvis_enum!(AssetClass: u8 { FX = 1 => "FX", Equity = 2 => "EQUITY", Commodity = 3 => "COMMODITY", Debt = 4 => "DEBT", Index = 5 => "INDEX", Cryptocurrency = 6 => "CRYPTOCURRENCY", Alternative = 7 => "ALTERNATIVE" });
jarvis_enum!(BarAggregation: u8 {
    Tick = 1 => "TICK", TickImbalance = 2 => "TICK_IMBALANCE", TickRuns = 3 => "TICK_RUNS",
    Volume = 4 => "VOLUME", VolumeImbalance = 5 => "VOLUME_IMBALANCE", VolumeRuns = 6 => "VOLUME_RUNS",
    Value = 7 => "VALUE", ValueImbalance = 8 => "VALUE_IMBALANCE", ValueRuns = 9 => "VALUE_RUNS",
    Millisecond = 10 => "MILLISECOND", Second = 11 => "SECOND", Minute = 12 => "MINUTE", Hour = 13 => "HOUR",
    Day = 14 => "DAY", Week = 15 => "WEEK", Month = 16 => "MONTH", Year = 17 => "YEAR", Renko = 18 => "RENKO",
});
jarvis_enum!(BarIntervalType: u8 { LeftOpen = 1 => "LEFT_OPEN", RightOpen = 2 => "RIGHT_OPEN" });
jarvis_enum!(BookAction: u8 { Add = 1 => "ADD", Update = 2 => "UPDATE", Delete = 3 => "DELETE", Clear = 4 => "CLEAR" });
jarvis_enum!(BookType: u8 { L1Mbp = 1 => "L1_MBP", L2Mbp = 2 => "L2_MBP", L3Mbo = 3 => "L3_MBO" });
jarvis_enum!(ContingencyType: u8 { Oco = 1 => "OCO", Oto = 2 => "OTO", Ouo = 3 => "OUO" });
jarvis_enum!(CurrencyType: u8 { Crypto = 1 => "CRYPTO", Fiat = 2 => "FIAT", CommodityBacked = 3 => "COMMODITY_BACKED" });
jarvis_enum!(InstrumentClass: u8 {
    Spot = 1 => "SPOT", Swap = 2 => "SWAP", Future = 3 => "FUTURE", FuturesSpread = 4 => "FUTURES_SPREAD",
    Forward = 5 => "FORWARD", Cfd = 6 => "CFD", Bond = 7 => "BOND", Option = 8 => "OPTION",
    OptionSpread = 9 => "OPTION_SPREAD", Warrant = 10 => "WARRANT", SportsBetting = 11 => "SPORTS_BETTING",
    BinaryOption = 12 => "BINARY_OPTION",
});
jarvis_enum!(InstrumentCloseType: u8 { EndOfSession = 1 => "END_OF_SESSION", ContractExpired = 2 => "CONTRACT_EXPIRED" });
jarvis_enum!(LiquiditySide: u8 { NoLiquiditySide = 0 => "NO_LIQUIDITY_SIDE", Maker = 1 => "MAKER", Taker = 2 => "TAKER" });
jarvis_enum!(MarketStatus: u8 { Open = 1 => "OPEN", Closed = 2 => "CLOSED", Paused = 3 => "PAUSED", Halted = 4 => "HALTED", Suspended = 5 => "SUSPENDED", NotAvailable = 6 => "NOT_AVAILABLE" });
jarvis_enum!(MarketStatusAction: u8 {
    None = 0 => "NONE", PreOpen = 1 => "PRE_OPEN", PreCross = 2 => "PRE_CROSS", Quoting = 3 => "QUOTING",
    Cross = 4 => "CROSS", Rotation = 5 => "ROTATION", NewPriceIndication = 6 => "NEW_PRICE_INDICATION",
    Trading = 7 => "TRADING", Halt = 8 => "HALT", Pause = 9 => "PAUSE", Suspend = 10 => "SUSPEND",
    PreClose = 11 => "PRE_CLOSE", Close = 12 => "CLOSE", PostClose = 13 => "POST_CLOSE",
    ShortSellRestrictionChange = 14 => "SHORT_SELL_RESTRICTION_CHANGE", NotAvailableForTrading = 15 => "NOT_AVAILABLE_FOR_TRADING",
});
jarvis_enum!(OmsType: u8 { Unspecified = 0 => "UNSPECIFIED", Netting = 1 => "NETTING", Hedging = 2 => "HEDGING" });
jarvis_enum!(OptionKind: u8 { Call = 1 => "CALL", Put = 2 => "PUT" });
jarvis_enum!(OrderSide: u8 { Buy = 1 => "BUY", Sell = 2 => "SELL" });
jarvis_enum!(OrderStatus: u8 {
    Initialized = 1 => "INITIALIZED", Denied = 2 => "DENIED", Emulated = 3 => "EMULATED", Released = 4 => "RELEASED",
    Submitted = 5 => "SUBMITTED", Accepted = 6 => "ACCEPTED", Rejected = 7 => "REJECTED", Canceled = 8 => "CANCELED",
    Expired = 9 => "EXPIRED", Triggered = 10 => "TRIGGERED", PendingUpdate = 11 => "PENDING_UPDATE",
    PendingCancel = 12 => "PENDING_CANCEL", PartiallyFilled = 13 => "PARTIALLY_FILLED", Filled = 14 => "FILLED", Voided = 15 => "VOIDED",
});
jarvis_enum!(OrderType: u8 {
    Market = 1 => "MARKET", Limit = 2 => "LIMIT", StopMarket = 3 => "STOP_MARKET", StopLimit = 4 => "STOP_LIMIT",
    MarketToLimit = 5 => "MARKET_TO_LIMIT", MarketIfTouched = 6 => "MARKET_IF_TOUCHED", LimitIfTouched = 7 => "LIMIT_IF_TOUCHED",
    TrailingStopMarket = 8 => "TRAILING_STOP_MARKET", TrailingStopLimit = 9 => "TRAILING_STOP_LIMIT",
});
jarvis_enum!(PositionAdjustmentType: u8 { Commission = 1 => "COMMISSION", Funding = 2 => "FUNDING" });
jarvis_enum!(PositionSide: u8 { Flat = 1 => "FLAT", Long = 2 => "LONG", Short = 3 => "SHORT" });
jarvis_enum!(PriceType: u8 { Bid = 1 => "BID", Ask = 2 => "ASK", Mid = 3 => "MID", Last = 4 => "LAST", Mark = 5 => "MARK" });
jarvis_enum!(
    /// Bit flags on order book records; combine with `|` on the `u8`.
    RecordFlag: u8 { Last = 128 => "F_LAST", Tob = 64 => "F_TOB", Snapshot = 32 => "F_SNAPSHOT", Mbp = 16 => "F_MBP", Reserved2 = 8 => "RESERVED_2", Reserved1 = 4 => "RESERVED_1" }
);
jarvis_enum!(TimeInForce: u8 { Gtc = 1 => "GTC", Ioc = 2 => "IOC", Fok = 3 => "FOK", Gtd = 4 => "GTD", Day = 5 => "DAY", AtTheOpen = 6 => "AT_THE_OPEN", AtTheClose = 7 => "AT_THE_CLOSE" });
jarvis_enum!(TradingState: u8 { Active = 1 => "ACTIVE", Reducing = 2 => "REDUCING", Halted = 3 => "HALTED" });
jarvis_enum!(TrailingOffsetType: u8 { Price = 1 => "PRICE", BasisPoints = 2 => "BASIS_POINTS", Ticks = 3 => "TICKS", PriceTier = 4 => "PRICE_TIER" });
jarvis_enum!(TriggerType: u8 {
    Default = 1 => "DEFAULT", LastPrice = 2 => "LAST_PRICE", MarkPrice = 3 => "MARK_PRICE", IndexPrice = 4 => "INDEX_PRICE",
    BidAsk = 5 => "BID_ASK", DoubleLast = 6 => "DOUBLE_LAST", DoubleBidAsk = 7 => "DOUBLE_BID_ASK", LastOrBidAsk = 8 => "LAST_OR_BID_ASK", MidPoint = 9 => "MID_POINT",
});

impl OrderSide {
    #[must_use]
    pub const fn opposite(self) -> OrderSide {
        match self {
            OrderSide::Buy => OrderSide::Sell,
            OrderSide::Sell => OrderSide::Buy,
        }
    }
}

impl RecordFlag {
    /// Whether `flags` carries this flag.
    #[must_use]
    pub const fn is_set(self, flags: u8) -> bool {
        flags & (self as u8) != 0
    }
}

jarvis_enum!(
    /// jarvis's own: how a node stops (docs/architecture.md section 19.4).
    StopMode: u8 { CancelAllThenExit = 1 => "CANCEL_ALL_THEN_EXIT", LeaveOrders = 2 => "LEAVE_ORDERS", KillSwitch = 3 => "KILL_SWITCH" });
