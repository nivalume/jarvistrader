//! The `nautilus_trader` compatibility contract (docs/architecture.md section 6), checked against
//! vectors generated from `tests/conformance/nautilus_cd417b80.json` by
//! `rust/tools/gen_conformance.py`.

mod generated {
    pub mod vectors;
}

use core::str::FromStr;

use generated::vectors::{
    EnumVectors, CURRENCIES, ENUMS, FIXED_PRECISION, LEGACY_NONE_TOKENS, MONEY_MAX_INTEGRAL,
    PRICE_MAX_INTEGRAL, QUANTITY_MAX_INTEGRAL, STRUCT_FIELDS, UNUSED_ENUMS,
};
use jarvis_core::Status;
use jarvis_model::enums::*;
use jarvis_model::fixed_point::{PRICE_RAW_MAX, QUANTITY_RAW_MAX};
use jarvis_model::money::MONEY_RAW_MAX;
use jarvis_model::{Currency, Price, Quantity, FIXED_SCALAR};

/// Checks one enum against its vectors: every variant's value and string, parsing of the string
/// and its aliases in any case, the legacy `NO_*` token refused, and no extra variants.
fn check_enum<E>(vectors: &EnumVectors)
where
    E: EnumInfo
        + core::fmt::Debug
        + PartialEq
        + core::fmt::Display
        + FromStr<Err = Status>
        + 'static,
{
    let all = enum_all::<E>();
    assert_eq!(all.len(), vectors.variants.len(), "{}: variant count", vectors.name);
    for (variant, expected) in all.iter().zip(vectors.variants) {
        assert_eq!(variant.to_string(), expected.string, "{}::{}", vectors.name, expected.name);
        assert_eq!(
            enum_value(*variant),
            expected.value,
            "{}::{} value",
            vectors.name,
            expected.name
        );
        assert_eq!(
            E::from_str(expected.string).ok(),
            Some(*variant),
            "{} parses {}",
            vectors.name,
            expected.string
        );
        if vectors.case_insensitive {
            assert_eq!(
                E::from_str(&expected.string.to_ascii_lowercase()).ok(),
                Some(*variant),
                "{} parses lower-case {}",
                vectors.name,
                expected.string
            );
        }
        for alias in expected.aliases {
            assert_eq!(
                E::from_str(alias).ok(),
                Some(*variant),
                "{} parses alias {alias}",
                vectors.name
            );
        }
    }
    if let Some((_, token)) = LEGACY_NONE_TOKENS.iter().find(|(name, _)| *name == vectors.name) {
        assert!(
            E::from_str(token).is_err(),
            "{} must refuse the legacy token {token}",
            vectors.name
        );
    }
    assert!(E::from_str("").is_err());
    assert!(E::from_str("NOT_A_VARIANT").is_err());
}

trait EnumInfo: Sized + Copy {
    fn all() -> &'static [Self];
    fn value_i64(self) -> i64;
}
macro_rules! enum_info {
    ($($t:ident),+ $(,)?) => {
        $(impl EnumInfo for $t {
            fn all() -> &'static [Self] { $t::ALL }
            fn value_i64(self) -> i64 { i64::from(self.value()) }
        })+
    };
}
enum_info!(
    AccountType,
    AggregationSource,
    AggressorSide,
    AssetClass,
    BarAggregation,
    BarIntervalType,
    BookAction,
    BookType,
    ContingencyType,
    CurrencyType,
    InstrumentClass,
    InstrumentCloseType,
    LiquiditySide,
    MarketStatus,
    MarketStatusAction,
    OmsType,
    OptionKind,
    OrderSide,
    OrderStatus,
    OrderType,
    PositionAdjustmentType,
    PositionSide,
    PriceType,
    RecordFlag,
    TimeInForce,
    TradingState,
    TrailingOffsetType,
    TriggerType
);
fn enum_all<E: EnumInfo>() -> &'static [E] {
    E::all()
}
fn enum_value<E: EnumInfo>(e: E) -> i64 {
    e.value_i64()
}

fn vectors_for(name: &str) -> &'static EnumVectors {
    ENUMS.iter().find(|e| e.name == name).unwrap_or_else(|| panic!("no vectors for {name}"))
}

#[test]
fn enums_match_nautilus_values_strings_and_aliases() {
    macro_rules! check {
        ($($t:ident),+ $(,)?) => { $( check_enum::<$t>(vectors_for(stringify!($t))); )+ };
    }
    check!(
        AccountType,
        AggregationSource,
        AggressorSide,
        AssetClass,
        BarAggregation,
        BarIntervalType,
        BookAction,
        BookType,
        ContingencyType,
        CurrencyType,
        InstrumentClass,
        InstrumentCloseType,
        LiquiditySide,
        MarketStatus,
        MarketStatusAction,
        OmsType,
        OptionKind,
        OrderSide,
        OrderStatus,
        OrderType,
        PositionAdjustmentType,
        PositionSide,
        PriceType,
        RecordFlag,
        TimeInForce,
        TradingState,
        TrailingOffsetType,
        TriggerType
    );
    // Every enum in the vectors is either defined here or listed as deliberately unused.
    assert_eq!(ENUMS.len() + UNUSED_ENUMS.len(), 32);
    assert_eq!(
        UNUSED_ENUMS,
        ["BetSide", "ContinuousFutureAdjustmentType", "GreeksConvention", "OtoTriggerMode"]
    );
}

#[test]
fn fixed_point_constants_match_nautilus() {
    assert_eq!(jarvis_model::FIXED_PRECISION, FIXED_PRECISION);
    assert_eq!(FIXED_SCALAR, 1_000_000_000);
    assert_eq!(PRICE_RAW_MAX, PRICE_MAX_INTEGRAL * FIXED_SCALAR);
    assert_eq!(QUANTITY_RAW_MAX, QUANTITY_MAX_INTEGRAL as u64 * FIXED_SCALAR as u64);
    assert_eq!(MONEY_RAW_MAX, MONEY_MAX_INTEGRAL * FIXED_SCALAR);
    assert_eq!(
        Price::from_raw(PRICE_RAW_MAX, 0).map(|p| p.to_string()),
        Ok("9223372036".to_string())
    );
    assert_eq!(Price::from_raw(PRICE_RAW_MAX + 1, 0), Err(Status::OutOfRange));
    assert_eq!(Price::from_raw(PRICE_RAW_MAX + 1, 9), Err(Status::OutOfRange));
    assert_eq!(
        Quantity::from_raw(QUANTITY_RAW_MAX, 0).map(|q| q.to_string()),
        Ok("18446744073".to_string())
    );
    assert_eq!(Quantity::from_raw(QUANTITY_RAW_MAX + 1, 9), Err(Status::OutOfRange));
}

#[test]
fn builtin_currencies_match_nautilus() {
    assert_eq!(Currency::builtins().len(), CURRENCIES.len());
    for (currency, expected) in Currency::builtins().iter().zip(CURRENCIES) {
        assert_eq!(currency.code(), expected.code);
        assert_eq!(currency.precision, expected.precision, "{}", expected.code);
        assert_eq!(currency.iso4217, expected.iso4217, "{}", expected.code);
        assert_eq!(currency.name.as_str(), expected.name, "{}", expected.code);
        // The vectors name the type by its Rust variant (`CommodityBacked`); compare variant names.
        assert_eq!(
            format!("{:?}", currency.currency_type),
            expected.currency_type,
            "{}",
            expected.code
        );
        assert_eq!(Currency::builtin_by_code(expected.code).as_ref(), Some(currency));
    }
    assert_eq!(Currency::builtin_by_code("XXX"), None);
}

/// The struct field lists are the contract for the data and event types. Fields nautilus carries
/// as free dictionaries or lists are the documented omissions (section 6.8).
#[test]
fn struct_field_lists_are_accounted_for() {
    const OMITTED: &[&str] =
        &["tick_scheme", "info", "linked_order_ids", "exec_algorithm_params", "tags"];
    let names: Vec<&str> = STRUCT_FIELDS.iter().map(|(n, _)| *n).collect();
    for expected in [
        "QuoteTick",
        "TradeTick",
        "Bar",
        "OrderBookDelta",
        "OrderBookDeltas",
        "CryptoPerpetual",
        "OrderFilled",
        "PositionClosed",
        "AccountState",
    ] {
        assert!(names.contains(&expected), "{expected} missing from the vectors");
    }
    let omitted_present: Vec<&str> = STRUCT_FIELDS
        .iter()
        .flat_map(|(_, f)| f.iter().copied())
        .filter(|f| OMITTED.contains(f))
        .collect();
    assert!(!omitted_present.is_empty(), "the vectors should name the omitted dictionaries");
}
