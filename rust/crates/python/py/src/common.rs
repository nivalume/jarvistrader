//! Conversions between Python objects and the kernel's value types, and the errors the extension
//! raises: a kernel `Status` becomes `ValueError("<what>: <Status>")`, a wrong Python type a
//! `TypeError`.

use kernel_core::{Result as KResult, Status, UnixNanos};
use model::identifiers::*;
use model::order_events::FillInfoFlags;
use model::wire::{Wire, WireReader, WireWriter};
use model::{Currency, Money, Price, Quantity, Uuid4};
use pyo3::exceptions::{PyTypeError, PyValueError};
use pyo3::prelude::*;
use pyo3::types::{PyBool, PyBoolMethods, PyDict, PyFloat, PyInt, PyList, PyString, PyTuple};

use crate::values::{
    PyAccountId, PyBarSpecification, PyBarType, PyBookOrder, PyClientId, PyClientOrderId,
    PyComponentId, PyCurrency, PyExecAlgorithmId, PyInstrumentId, PyMoney, PyOrderListId,
    PyPositionId, PyPrice, PyQuantity, PyStrategyId, PySymbol, PyTradeId, PyTraderId, PyUuid4,
    PyVenue, PyVenueOrderId,
};

/// `ValueError("<what>: <status>")`.
pub(crate) fn status_error(what: &str, status: Status) -> PyErr {
    PyValueError::new_err(format!("{what}: {status}"))
}

/// Maps a kernel result to a Python one.
pub(crate) fn check<T>(result: KResult<T>, what: &str) -> PyResult<T> {
    result.map_err(|s| status_error(what, s))
}

/// `TypeError("<what>: expected <expected>, got <type>")`.
pub(crate) fn type_error(what: &str, expected: &str, got: &Bound<'_, PyAny>) -> PyErr {
    let name = got.get_type().name().map_or_else(|_| "?".to_string(), |n| n.to_string());
    PyTypeError::new_err(format!("{what}: expected {expected}, got {name}"))
}

/// A `decimal.Decimal` from decimal text.
pub(crate) fn python_decimal<'py>(py: Python<'py>, text: &str) -> PyResult<Bound<'py, PyAny>> {
    py.import("decimal")?.getattr("Decimal")?.call1((text,))
}

/// A `decimal.Decimal` of a raw value on the 1e9 scale.
pub(crate) fn decimal9(py: Python<'_>, raw: i64) -> PyResult<Bound<'_, PyAny>> {
    let mut buf = [0u8; 48];
    let n = model::decimal::format_raw(i128::from(raw), 9, &mut buf);
    let text = core::str::from_utf8(&buf[..n]).unwrap_or("0");
    // Trailing zeros carry no information at this scale; `Decimal("1.5")` reads better.
    let trimmed = match text.find('.') {
        Some(_) => text.trim_end_matches('0').trim_end_matches('.'),
        None => text,
    };
    python_decimal(py, trimmed)
}

fn is_decimal(obj: &Bound<'_, PyAny>) -> bool {
    obj.hasattr("as_tuple").unwrap_or(false)
}

/// Decimal text from a str, int, `decimal.Decimal` or float (shortest repr).
pub(crate) fn decimal_text(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<String> {
    if let Ok(s) = obj.cast::<PyString>() {
        return Ok(s.to_str()?.to_string());
    }
    if obj.is_instance_of::<PyBool>() {
        return Err(type_error(what, "a decimal text, int or decimal.Decimal", obj));
    }
    if obj.is_instance_of::<PyInt>() || is_decimal(obj) || obj.is_instance_of::<PyFloat>() {
        let text = obj.str()?.to_str()?.to_string();
        // Decimal("1E+2") style exponents are not in the kernel's grammar.
        if text.contains('E') || text.contains('e') {
            let fixed = obj.call_method1("__format__", ("f",))?;
            return Ok(fixed.str()?.to_str()?.to_string());
        }
        return Ok(text);
    }
    Err(type_error(what, "a decimal text, int or decimal.Decimal", obj))
}

/// A raw value on the 1e9 scale from a str, int or Decimal (floats refused).
pub(crate) fn raw9(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<i64> {
    if obj.is_instance_of::<PyFloat>() {
        return Err(type_error(what, "a decimal text, int or decimal.Decimal (not a float)", obj));
    }
    let text = decimal_text(obj, what)?;
    let d = check(model::decimal::parse(&text), what)?;
    let raw = check(model::decimal::to_raw(&d, 9), what)?;
    i64::try_from(raw).map_err(|_| status_error(what, Status::OutOfRange))
}

/// Validates `value` by its wire round trip: the decoder runs the type's constructor.
pub(crate) fn validated<T: Wire>(value: &T, what: &str) -> PyResult<T> {
    let mut w = WireWriter::new();
    value.encode(&mut w);
    let mut r = WireReader::new(w.as_slice());
    let out = check(T::decode(&mut r), what)?;
    check(r.finish(), what)?;
    Ok(out)
}

/// A Python bool.
pub(crate) fn py_bool(py: Python<'_>, b: bool) -> Bound<'_, PyAny> {
    PyBool::new(py, b).to_owned().into_any()
}

/// A field of an event as Python sees it.
pub(crate) trait PyField: Sized {
    /// `Some` when a missing argument has a default (`None`, `False`).
    fn default_py(py: Python<'_>) -> Option<Py<PyAny>> {
        let _ = py;
        None
    }
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>>;
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self>;
}

macro_rules! int_field {
    ($($t:ty),+) => {$(
        impl PyField for $t {
            fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
                Ok((*self).into_pyobject(py)?.into_any())
            }
            fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
                if obj.is_instance_of::<PyBool>() || !obj.is_instance_of::<PyInt>() {
                    return Err(type_error(what, "an int", obj));
                }
                let wide: i128 = obj.extract()?;
                <$t>::try_from(wide).map_err(|_| status_error(what, Status::OutOfRange))
            }
        }
    )+};
}
int_field!(u8, u16, u32, u64, i64);

impl PyField for bool {
    fn default_py(py: Python<'_>) -> Option<Py<PyAny>> {
        Some(py_bool(py, false).unbind())
    }
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(py_bool(py, *self))
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        obj.cast::<PyBool>()
            .map(PyBoolMethods::is_true)
            .map_err(|_| type_error(what, "a bool", obj))
    }
}

impl PyField for UnixNanos {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.value().to_py(py)
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        Ok(UnixNanos::new(u64::from_py(obj, what)?))
    }
}

impl PyField for kernel_core::DurationNanos {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.value().to_py(py)
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        Ok(kernel_core::DurationNanos::new(u64::from_py(obj, what)?))
    }
}

impl<T: PyField> PyField for Option<T> {
    fn default_py(py: Python<'_>) -> Option<Py<PyAny>> {
        Some(py.None())
    }
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        match self {
            None => Ok(py.None().into_bound(py)),
            Some(v) => v.to_py(py),
        }
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if obj.is_none() {
            return Ok(None);
        }
        T::from_py(obj, what).map(Some)
    }
}

impl<const N: usize> PyField for kernel_core::FixedString<N> {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(PyString::new(py, self.as_str()).into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        let s = obj.cast::<PyString>().map_err(|_| type_error(what, "a str", obj))?;
        check(kernel_core::FixedString::from_text(s.to_str()?), what)
    }
}

impl<T: PyField> PyField for kernel_core::FixedVec<T> {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let items: Vec<Bound<'py, PyAny>> =
            self.iter().map(|v| v.to_py(py)).collect::<PyResult<_>>()?;
        Ok(PyList::new(py, items)?.into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        let items: Vec<Bound<'_, PyAny>> = if let Ok(list) = obj.cast::<PyList>() {
            list.iter().collect()
        } else if let Ok(tuple) = obj.cast::<PyTuple>() {
            tuple.iter().collect()
        } else {
            return Err(type_error(what, "a list", obj));
        };
        let mut out = kernel_core::FixedVec::with_capacity(items.len());
        for item in &items {
            check(out.push(T::from_py(item, what)?), what)?;
        }
        Ok(out)
    }
}

impl PyField for FillInfoFlags {
    fn default_py(py: Python<'_>) -> Option<Py<PyAny>> {
        0u8.into_pyobject(py).map(|b| b.into_any().unbind()).ok()
    }
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.to_py(py)
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        check(FillInfoFlags::new(u8::from_py(obj, what)?), what)
    }
}

/// A raw 1e9-scale integer shown to Python as `decimal.Decimal`.
pub(crate) struct Dec9(pub i64);
impl PyField for Dec9 {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        decimal9(py, self.0)
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        raw9(obj, what).map(Dec9)
    }
}

/// A `Price` shown to Python as `decimal.Decimal` (rates).
pub(crate) struct DecPrice(pub Price);
impl PyField for DecPrice {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        python_decimal(py, &self.0.to_string())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if obj.is_instance_of::<PyFloat>() {
            return Err(type_error(what, "a decimal text, int or decimal.Decimal", obj));
        }
        check(Price::parse(&decimal_text(obj, what)?), what).map(DecPrice)
    }
}

/// A `Quantity` shown to Python as `decimal.Decimal` (margin rates).
pub(crate) struct DecQuantity(pub Quantity);
impl PyField for DecQuantity {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        python_decimal(py, &self.0.to_string())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if obj.is_instance_of::<PyFloat>() {
            return Err(type_error(what, "a decimal text, int or decimal.Decimal", obj));
        }
        check(Quantity::parse(&decimal_text(obj, what)?), what).map(DecQuantity)
    }
}

macro_rules! wrapped_field {
    ($t:ty, $wrapper:ident, $expected:literal, $parse:expr) => {
        impl PyField for $t {
            fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
                Ok(Bound::new(py, $wrapper(*self))?.into_any())
            }
            fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
                if let Ok(v) = obj.cast::<$wrapper>() {
                    return Ok(v.get().0);
                }
                if let Ok(s) = obj.cast::<PyString>() {
                    let text = s.to_str()?;
                    #[allow(clippy::redundant_closure_call)]
                    return check(($parse)(text), what);
                }
                Err(type_error(what, $expected, obj))
            }
        }
    };
}

impl PyField for Price {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(Bound::new(py, PyPrice(*self))?.into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if let Ok(v) = obj.cast::<PyPrice>() {
            return Ok(v.get().0);
        }
        if obj.is_instance_of::<PyFloat>() {
            return Err(type_error(what, "a Price (a float needs Price.from_float)", obj));
        }
        if obj.is_instance_of::<PyString>() || obj.is_instance_of::<PyInt>() || is_decimal(obj) {
            return check(Price::parse(&decimal_text(obj, what)?), what);
        }
        Err(type_error(what, "a Price or decimal text", obj))
    }
}

impl PyField for Quantity {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(Bound::new(py, PyQuantity(*self))?.into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if let Ok(v) = obj.cast::<PyQuantity>() {
            return Ok(v.get().0);
        }
        if obj.is_instance_of::<PyFloat>() {
            return Err(type_error(what, "a Quantity (a float needs Quantity.from_float)", obj));
        }
        if obj.is_instance_of::<PyString>() || obj.is_instance_of::<PyInt>() || is_decimal(obj) {
            return check(Quantity::parse(&decimal_text(obj, what)?), what);
        }
        Err(type_error(what, "a Quantity or decimal text", obj))
    }
}

wrapped_field!(Money, PyMoney, "a Money or text such as \"10 USDT\"", Money::parse);
wrapped_field!(
    InstrumentId,
    PyInstrumentId,
    "an InstrumentId or \"SYMBOL.VENUE\"",
    InstrumentId::parse
);
wrapped_field!(Uuid4, PyUuid4, "a UUID4 or its text", Uuid4::parse);
wrapped_field!(model::bar::BarType, PyBarType, "a BarType or its text", model::bar::BarType::parse);
wrapped_field!(
    model::bar::BarSpecification,
    PyBarSpecification,
    "a BarSpecification or its text",
    model::bar::BarSpecification::parse
);

impl PyField for Currency {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(Bound::new(py, PyCurrency(*self))?.into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        if let Ok(v) = obj.cast::<PyCurrency>() {
            return Ok(v.get().0);
        }
        if let Ok(s) = obj.cast::<PyString>() {
            let code = s.to_str()?;
            return Currency::builtin_by_code(code)
                .ok_or_else(|| status_error(what, Status::NotFound));
        }
        Err(type_error(what, "a Currency or a built-in code", obj))
    }
}

impl PyField for model::data::BookOrder {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(Bound::new(py, PyBookOrder(*self))?.into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        obj.cast::<PyBookOrder>()
            .map(|v| v.get().0)
            .map_err(|_| type_error(what, "a BookOrder", obj))
    }
}

macro_rules! id_field {
    ($($t:ident => $wrapper:ident),+ $(,)?) => {$(
        impl PyField for $t {
            fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
                Ok(Bound::new(py, $wrapper(*self))?.into_any())
            }
            fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
                if let Ok(v) = obj.cast::<$wrapper>() {
                    return Ok(v.get().0);
                }
                if let Ok(s) = obj.cast::<PyString>() {
                    return check($t::new(s.to_str()?), what);
                }
                Err(type_error(what, concat!("a ", stringify!($t), " or str"), obj))
            }
        }
    )+};
}
id_field!(
    Symbol => PySymbol, Venue => PyVenue, TraderId => PyTraderId, StrategyId => PyStrategyId,
    AccountId => PyAccountId, ClientOrderId => PyClientOrderId, VenueOrderId => PyVenueOrderId,
    ClientId => PyClientId, ComponentId => PyComponentId, ExecAlgorithmId => PyExecAlgorithmId,
    OrderListId => PyOrderListId, TradeId => PyTradeId, PositionId => PyPositionId,
);

/// The Python enum classes live in `jarvis._core.model`; a Rust enum value becomes the member
/// of that class with the same integer value.
pub(crate) fn enum_to_py<'py>(
    py: Python<'py>,
    class: &str,
    value: u8,
) -> PyResult<Bound<'py, PyAny>> {
    crate::model::enum_class(py, class)?.call1((value,))
}

pub(crate) fn enum_from_py(obj: &Bound<'_, PyAny>, class: &str, what: &str) -> PyResult<u8> {
    if let Ok(s) = obj.cast::<PyString>() {
        let member = crate::model::enum_class(obj.py(), class)?.call_method1("from_str", (s,))?;
        return member.getattr("value")?.extract::<u8>();
    }
    let cls = crate::model::enum_class(obj.py(), class)?;
    if !obj.is_instance(&cls)? {
        return Err(type_error(what, &format!("a {class}"), obj));
    }
    obj.getattr("value")?.extract::<u8>()
}

macro_rules! enum_field {
    ($($t:ident),+ $(,)?) => {$(
        impl PyField for model::enums::$t {
            fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
                enum_to_py(py, stringify!($t), self.value())
            }
            fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
                let v = enum_from_py(obj, stringify!($t), what)?;
                check(model::enums::$t::from_value(v), what)
            }
        }
    )+};
}
enum_field!(
    AccountType,
    AggregationSource,
    AggressorSide,
    AssetClass,
    BarAggregation,
    BetSide,
    BookAction,
    ContinuousFutureAdjustmentType,
    GreeksConvention,
    OtoTriggerMode,
    BookType,
    ContingencyType,
    CurrencyType,
    InstrumentClass,
    InstrumentCloseType,
    LiquiditySide,
    MarketStatusAction,
    OrderSide,
    OrderStatus,
    OrderType,
    PositionAdjustmentType,
    PositionSide,
    PriceType,
    TimeInForce,
    TradingState,
    TrailingOffsetType,
    TriggerType,
    NodeState,
    LifecycleReason,
    StopMode,
    StrategyErrorKind,
);

/// `kwargs` as a dict of `(name, value)`.
pub(crate) fn dict_of<'py>(
    py: Python<'py>,
    items: &[(&str, Bound<'py, PyAny>)],
) -> PyResult<Bound<'py, PyDict>> {
    let d = PyDict::new(py);
    for (k, v) in items {
        d.set_item(*k, v)?;
    }
    Ok(d)
}

impl PyField for kernel_core::clock::TimerKey {
    fn to_py<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        Ok(PyTuple::new(py, [self.owner, self.id])?.into_any())
    }
    fn from_py(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<Self> {
        let (owner, id): (u32, u32) =
            obj.extract().map_err(|_| type_error(what, "a (owner, id) pair", obj))?;
        Ok(kernel_core::clock::TimerKey::new(owner, id))
    }
}
