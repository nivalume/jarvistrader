//! Python classes for the model's value types: `Price`, `Quantity`, `Money`, `Currency`, the
//! identifiers, `UUID4`, `BarSpecification`, `BarType`, `BookOrder` and the `ClientOrderId`
//! generator. Values are immutable and copied across the boundary.

use kernel_core::rng::CounterRng;
use model::bar::{BarSpecification, BarType};
use model::client_order_id::Generator;
use model::data::BookOrder;
use model::enums::{AggregationSource, BarAggregation, CurrencyType, OrderSide, PriceType};
use model::identifiers::*;
use model::{Currency, Money, Price, Quantity, Uuid4};
use pyo3::basic::CompareOp;
use pyo3::exceptions::PyTypeError;
use pyo3::prelude::*;
use pyo3::types::{PyFloat, PyInt};

use crate::common::{check, decimal_text, py_bool, python_decimal, type_error, PyField};

fn crc_hash(text: &str) -> isize {
    kernel_core::crc32c::crc32c(text.as_bytes()) as isize
}

fn float_text(value: &Bound<'_, PyAny>, what: &str) -> PyResult<String> {
    if value.is_instance_of::<PyFloat>() {
        return Ok(value.repr()?.to_str()?.to_string());
    }
    if value.is_instance_of::<PyInt>() {
        return Ok(value.str()?.to_str()?.to_string());
    }
    Err(type_error(what, "a float or int", value))
}

macro_rules! fixed_point_class {
    ($py:ident, $t:ty, $raw:ty, $name:literal, $doc:literal) => {
        #[pyclass(frozen, name = $name, module = "jarvis._core.model")]
        #[doc = $doc]
        #[derive(Debug)]
        pub struct $py(pub $t);

        #[pymethods]
        impl $py {
            #[new]
            #[pyo3(signature = (value, precision = None))]
            fn new(value: &Bound<'_, PyAny>, precision: Option<u8>) -> PyResult<Self> {
                if value.is_instance_of::<PyFloat>() && precision.is_none() {
                    return Err(PyTypeError::new_err(concat!(
                        $name,
                        "(): a float needs an explicit precision to be quantized"
                    )));
                }
                let text = if value.is_instance_of::<PyFloat>() {
                    float_text(value, $name)?
                } else {
                    decimal_text(value, $name)?
                };
                let v = match precision {
                    Some(p) => <$t>::parse_with_precision(&text, p),
                    None => <$t>::parse(&text),
                };
                Ok(Self(check(v, $name)?))
            }
            #[staticmethod]
            fn from_str(text: &str) -> PyResult<Self> {
                Ok(Self(check(<$t>::parse(text), $name)?))
            }
            /// Quantizes a float to `precision` digits, rounding half to even.
            #[staticmethod]
            fn from_float(value: &Bound<'_, PyAny>, precision: u8) -> PyResult<Self> {
                let text = float_text(value, $name)?;
                Ok(Self(check(<$t>::parse_with_precision(&text, precision), $name)?))
            }
            #[staticmethod]
            fn from_raw(raw: $raw, precision: u8) -> PyResult<Self> {
                Ok(Self(check(<$t>::from_raw(raw, precision), $name)?))
            }
            #[getter]
            fn raw(&self) -> $raw {
                self.0.raw()
            }
            #[getter]
            fn precision(&self) -> u8 {
                self.0.precision()
            }
            fn as_decimal<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
                python_decimal(py, &self.0.to_string())
            }
            fn as_double(&self) -> f64 {
                self.0.to_string().parse().unwrap_or(f64::NAN)
            }
            fn is_zero(&self) -> bool {
                self.0.is_zero()
            }
            fn __str__(&self) -> String {
                self.0.to_string()
            }
            fn __repr__(&self) -> String {
                format!("{}('{}')", $name, self.0)
            }
            fn __richcmp__<'py>(
                &self,
                other: &Bound<'py, PyAny>,
                op: CompareOp,
                py: Python<'py>,
            ) -> PyResult<Bound<'py, PyAny>> {
                let Ok(other) = other.cast::<Self>() else {
                    return Ok(match op {
                        CompareOp::Eq => py_bool(py, false),
                        CompareOp::Ne => py_bool(py, true),
                        _ => py.NotImplemented().into_bound(py),
                    });
                };
                let b = op.matches(self.0.raw().cmp(&other.get().0.raw()));
                Ok(py_bool(py, b))
            }
            fn __hash__(&self) -> isize {
                self.0.raw() as isize
            }
            fn __add__(&self, other: &Self) -> PyResult<Self> {
                Ok(Self(check(self.0.checked_add(other.0), $name)?))
            }
            fn __sub__(&self, other: &Self) -> PyResult<Self> {
                Ok(Self(check(self.0.checked_sub(other.0), $name)?))
            }
            fn __reduce__<'py>(
                slf: &Bound<'py, Self>,
            ) -> PyResult<(Bound<'py, PyAny>, ($raw, u8))> {
                let v = slf.get().0;
                Ok((slf.get_type().getattr("from_raw")?, (v.raw(), v.precision())))
            }
        }
    };
}

fixed_point_class!(PyPrice, Price, i64, "Price", "Signed price: raw int64 at 10^9 scale.");
fixed_point_class!(
    PyQuantity,
    Quantity,
    u64,
    "Quantity",
    "Non-negative quantity: raw uint64 at 10^9 scale."
);

/// An amount of a currency on the currency's precision grid.
#[pyclass(frozen, name = "Money", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyMoney(pub Money);

#[pymethods]
impl PyMoney {
    #[new]
    fn new(amount: &Bound<'_, PyAny>, currency: &Bound<'_, PyAny>) -> PyResult<Self> {
        let c = Currency::from_py(currency, "currency")?;
        let text = if amount.is_instance_of::<PyFloat>() {
            float_text(amount, "Money")?
        } else {
            decimal_text(amount, "Money")?
        };
        Ok(Self(check(Money::parse_amount(&text, c), "Money")?))
    }
    #[staticmethod]
    fn from_str(text: &str) -> PyResult<Self> {
        Ok(Self(check(Money::parse(text), "Money")?))
    }
    #[staticmethod]
    fn from_raw(raw: i64, currency: &Bound<'_, PyAny>) -> PyResult<Self> {
        let c = Currency::from_py(currency, "currency")?;
        Ok(Self(check(Money::from_raw(raw, c), "Money")?))
    }
    #[getter]
    fn raw(&self) -> i64 {
        self.0.raw()
    }
    #[getter]
    fn currency(&self) -> PyCurrency {
        PyCurrency(self.0.currency())
    }
    fn amount_text(&self) -> String {
        let text = self.0.to_string();
        text.split(' ').next().unwrap_or("0").to_string()
    }
    fn as_decimal<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        python_decimal(py, &self.amount_text())
    }
    fn as_double(&self) -> f64 {
        self.amount_text().parse().unwrap_or(f64::NAN)
    }
    fn is_zero(&self) -> bool {
        self.0.is_zero()
    }
    fn __str__(&self) -> String {
        self.0.to_string()
    }
    fn __repr__(&self) -> String {
        format!("Money('{}')", self.0)
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let Ok(other) = other.cast::<Self>() else {
            return Ok(match op {
                CompareOp::Eq => py_bool(py, false),
                CompareOp::Ne => py_bool(py, true),
                _ => py.NotImplemented().into_bound(py),
            });
        };
        let o = other.get().0;
        let same = self.0.currency() == o.currency();
        let b = match op {
            CompareOp::Eq => same && self.0.raw() == o.raw(),
            CompareOp::Ne => !(same && self.0.raw() == o.raw()),
            _ => {
                if !same {
                    return Err(PyTypeError::new_err("Money: currencies differ"));
                }
                op.matches(self.0.raw().cmp(&o.raw()))
            }
        };
        Ok(py_bool(py, b))
    }
    fn __hash__(&self) -> isize {
        crc_hash(&self.0.to_string())
    }
    fn __add__(&self, other: &Self) -> PyResult<Self> {
        Ok(Self(check(self.0.checked_add(other.0), "Money")?))
    }
    fn __sub__(&self, other: &Self) -> PyResult<Self> {
        Ok(Self(check(self.0.checked_sub(other.0), "Money")?))
    }
    fn __neg__(&self) -> Self {
        Self(self.0.negated())
    }
    fn __reduce__<'py>(slf: &Bound<'py, Self>) -> PyResult<(Bound<'py, PyAny>, (String,))> {
        Ok((slf.get_type().getattr("from_str")?, (slf.get().0.to_string(),)))
    }
}

/// A currency; equality is by code.
#[pyclass(frozen, name = "Currency", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyCurrency(pub Currency);

#[pymethods]
impl PyCurrency {
    #[new]
    fn new(
        code: &str,
        precision: u8,
        iso4217: u16,
        name: &str,
        currency_type: &Bound<'_, PyAny>,
    ) -> PyResult<Self> {
        let t = CurrencyType::from_py(currency_type, "currency_type")?;
        Ok(Self(check(Currency::new(code, precision, iso4217, name, t), "Currency")?))
    }
    /// A built-in nautilus currency by code.
    #[staticmethod]
    fn from_str(code: &str) -> PyResult<Self> {
        Currency::builtin_by_code(code)
            .map(Self)
            .ok_or_else(|| crate::common::status_error("Currency", kernel_core::Status::NotFound))
    }
    #[getter]
    fn code(&self) -> String {
        self.0.code().to_string()
    }
    #[getter]
    fn precision(&self) -> u8 {
        self.0.precision
    }
    #[getter]
    fn iso4217(&self) -> u16 {
        self.0.iso4217
    }
    #[getter]
    fn name(&self) -> String {
        self.0.name.as_str().to_string()
    }
    #[getter]
    fn currency_type<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.currency_type.to_py(py)
    }
    fn __str__(&self) -> String {
        self.0.code().to_string()
    }
    fn __repr__(&self) -> String {
        format!("Currency('{}')", self.0.code())
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        eq_only(py, op, other.cast::<Self>().ok().map(|o| o.get().0 == self.0))
    }
    fn __hash__(&self) -> isize {
        crc_hash(self.0.code())
    }
    fn __reduce__<'py>(
        slf: &Bound<'py, Self>,
    ) -> PyResult<(Bound<'py, PyAny>, (String, u8, u16, String, Bound<'py, PyAny>))> {
        let c = slf.get().0;
        Ok((
            slf.get_type().into_any(),
            (
                c.code().to_string(),
                c.precision,
                c.iso4217,
                c.name.as_str().to_string(),
                c.currency_type.to_py(slf.py())?,
            ),
        ))
    }
}

/// `Eq`/`Ne` against a maybe-foreign object; other comparisons are `NotImplemented`.
pub(crate) fn eq_only(py: Python<'_>, op: CompareOp, equal: Option<bool>) -> Bound<'_, PyAny> {
    match (op, equal) {
        (CompareOp::Eq, Some(e)) => py_bool(py, e),
        (CompareOp::Ne, Some(e)) => py_bool(py, !e),
        (CompareOp::Eq, None) => py_bool(py, false),
        (CompareOp::Ne, None) => py_bool(py, true),
        _ => py.NotImplemented().into_bound(py),
    }
}

macro_rules! identifier_class {
    ($py:ident, $t:ident, $name:literal, $doc:literal $(, { $($extra:item)* })?) => {
        #[pyclass(frozen, name = $name, module = "jarvis._core.model")]
        #[doc = $doc]
        #[derive(Debug)]
        pub struct $py(pub $t);

        #[pymethods]
        impl $py {
            #[new]
            fn new(value: &str) -> PyResult<Self> {
                Ok(Self(check($t::new(value), stringify!($t))?))
            }
            #[staticmethod]
            fn from_str(value: &str) -> PyResult<Self> {
                Ok(Self(check($t::new(value), stringify!($t))?))
            }
            #[getter]
            fn value(&self) -> String {
                self.0.as_str().to_string()
            }
            fn __str__(&self) -> String {
                self.0.as_str().to_string()
            }
            fn __repr__(&self) -> String {
                format!("{}('{}')", stringify!($t), self.0.as_str())
            }
            fn __richcmp__<'py>(
                &self,
                other: &Bound<'py, PyAny>,
                op: CompareOp,
                py: Python<'py>,
            ) -> PyResult<Bound<'py, PyAny>> {
                let Ok(other) = other.cast::<Self>() else {
                    return Ok(eq_only(py, op, None));
                };
                let b = op.matches(self.0.as_str().cmp(other.get().0.as_str()));
                Ok(py_bool(py, b))
            }
            fn __hash__(&self) -> isize {
                crc_hash(self.0.as_str())
            }
            fn __reduce__<'py>(slf: &Bound<'py, Self>) -> PyResult<(Bound<'py, PyAny>, (String,))> {
                Ok((slf.get_type().into_any(), (slf.get().0.as_str().to_string(),)))
            }
            $($($extra)*)?
        }
    };
}

identifier_class!(PySymbol, Symbol, "Symbol", "Venue symbol of an instrument, e.g. BTCUSDT-PERP.");
identifier_class!(PyVenue, Venue, "Venue", "Trading venue, e.g. BINANCE.");
identifier_class!(PyTraderId, TraderId, "TraderId", "\"{name}-{tag}\".", {
    fn get_tag(&self) -> String {
        self.0.tag().to_string()
    }
});
identifier_class!(PyStrategyId, StrategyId, "StrategyId", "\"{name}-{tag}\".", {
    fn get_tag(&self) -> String {
        self.0.tag().to_string()
    }
});
identifier_class!(PyAccountId, AccountId, "AccountId", "\"{issuer}-{number}\".", {
    fn get_issuer(&self) -> String {
        self.0.issuer().to_string()
    }
    fn get_id(&self) -> String {
        self.0.number().to_string()
    }
});
identifier_class!(PyClientOrderId, ClientOrderId, "ClientOrderId", "Client-assigned order id.");
identifier_class!(PyVenueOrderId, VenueOrderId, "VenueOrderId", "Venue-assigned order id.");
identifier_class!(PyClientId, ClientId, "ClientId", "Data or execution client id.");
identifier_class!(PyComponentId, ComponentId, "ComponentId", "Component id.");
identifier_class!(PyExecAlgorithmId, ExecAlgorithmId, "ExecAlgorithmId", "Execution algorithm id.");
identifier_class!(PyOrderListId, OrderListId, "OrderListId", "Order list id.");
identifier_class!(PyTradeId, TradeId, "TradeId", "Venue trade id (at most 36 ASCII characters).");
identifier_class!(PyPositionId, PositionId, "PositionId", "Position id.");

/// `"{symbol}.{venue}"`, split at the last dot.
#[pyclass(frozen, name = "InstrumentId", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyInstrumentId(pub InstrumentId);

#[pymethods]
impl PyInstrumentId {
    #[new]
    fn new(symbol: &Bound<'_, PyAny>, venue: &Bound<'_, PyAny>) -> PyResult<Self> {
        let s = Symbol::from_py(symbol, "symbol")?;
        let v = Venue::from_py(venue, "venue")?;
        Ok(Self(InstrumentId { symbol: s, venue: v }))
    }
    #[staticmethod]
    fn from_str(value: &str) -> PyResult<Self> {
        Ok(Self(check(InstrumentId::parse(value), "InstrumentId")?))
    }
    #[getter]
    fn symbol(&self) -> PySymbol {
        PySymbol(self.0.symbol)
    }
    #[getter]
    fn venue(&self) -> PyVenue {
        PyVenue(self.0.venue)
    }
    #[getter]
    fn value(&self) -> String {
        self.0.to_string()
    }
    fn __str__(&self) -> String {
        self.0.to_string()
    }
    fn __repr__(&self) -> String {
        format!("InstrumentId('{}')", self.0)
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        let Ok(other) = other.cast::<Self>() else { return eq_only(py, op, None) };
        let b = op.matches(self.0.to_string().cmp(&other.get().0.to_string()));
        py_bool(py, b)
    }
    fn __hash__(&self) -> isize {
        crc_hash(&self.0.to_string())
    }
    fn __reduce__<'py>(slf: &Bound<'py, Self>) -> PyResult<(Bound<'py, PyAny>, (String,))> {
        Ok((slf.get_type().getattr("from_str")?, (slf.get().0.to_string(),)))
    }
}

/// RFC 4122 version 4 UUID. There is no random constructor: ids come from text or from
/// `UUID4.derive` (deterministic).
#[pyclass(frozen, name = "UUID4", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyUuid4(pub Uuid4);

#[pymethods]
impl PyUuid4 {
    #[new]
    fn new(value: &str) -> PyResult<Self> {
        Ok(Self(check(Uuid4::parse(value), "UUID4")?))
    }
    #[staticmethod]
    fn from_str(value: &str) -> PyResult<Self> {
        Ok(Self(check(Uuid4::parse(value), "UUID4")?))
    }
    /// The UUID drawn from the counter-based RNG at (seed, identity, hop).
    #[staticmethod]
    fn derive(seed: u64, identity: u64, hop: u32) -> Self {
        let rng = CounterRng::new(seed);
        Self(Uuid4::from_u64s(rng.draw(identity, hop, 0), rng.draw(identity, hop, 1)))
    }
    #[getter]
    fn value(&self) -> String {
        self.0.to_string()
    }
    fn __str__(&self) -> String {
        self.0.to_string()
    }
    fn __repr__(&self) -> String {
        format!("UUID4('{}')", self.0)
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        eq_only(py, op, other.cast::<Self>().ok().map(|o| o.get().0 == self.0))
    }
    fn __hash__(&self) -> isize {
        crc_hash(&self.0.to_string())
    }
    fn __reduce__<'py>(slf: &Bound<'py, Self>) -> (Bound<'py, PyAny>, (String,)) {
        (slf.get_type().into_any(), (slf.get().0.to_string(),))
    }
}

/// `"{step}-{AGGREGATION}-{PRICE_TYPE}"`.
#[pyclass(frozen, name = "BarSpecification", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyBarSpecification(pub BarSpecification);

#[pymethods]
impl PyBarSpecification {
    #[new]
    fn new(
        step: u32,
        aggregation: &Bound<'_, PyAny>,
        price_type: &Bound<'_, PyAny>,
    ) -> PyResult<Self> {
        let a = BarAggregation::from_py(aggregation, "aggregation")?;
        let p = PriceType::from_py(price_type, "price_type")?;
        Ok(Self(check(BarSpecification::new(step, a, p), "BarSpecification")?))
    }
    #[staticmethod]
    fn from_str(value: &str) -> PyResult<Self> {
        Ok(Self(check(BarSpecification::parse(value), "BarSpecification")?))
    }
    #[getter]
    fn step(&self) -> u32 {
        self.0.step
    }
    #[getter]
    fn aggregation<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.aggregation.to_py(py)
    }
    #[getter]
    fn price_type<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.price_type.to_py(py)
    }
    fn __str__(&self) -> String {
        self.0.to_string()
    }
    fn __repr__(&self) -> String {
        format!("BarSpecification('{}')", self.0)
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        eq_only(py, op, other.cast::<Self>().ok().map(|o| o.get().0 == self.0))
    }
    fn __hash__(&self) -> isize {
        crc_hash(&self.0.to_string())
    }
    fn __reduce__<'py>(slf: &Bound<'py, Self>) -> PyResult<(Bound<'py, PyAny>, (String,))> {
        Ok((slf.get_type().getattr("from_str")?, (slf.get().0.to_string(),)))
    }
}

/// `"{instrument_id}-{spec}-{SOURCE}"`, optionally `"@{step}-{AGG}-{SOURCE}"` for a composite
/// bar type.
#[pyclass(frozen, name = "BarType", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyBarType(pub BarType);

#[pymethods]
impl PyBarType {
    #[new]
    #[pyo3(signature = (instrument_id, spec, aggregation_source))]
    fn new(
        instrument_id: &Bound<'_, PyAny>,
        spec: &Bound<'_, PyAny>,
        aggregation_source: &Bound<'_, PyAny>,
    ) -> PyResult<Self> {
        Ok(Self(BarType {
            instrument_id: InstrumentId::from_py(instrument_id, "instrument_id")?,
            spec: BarSpecification::from_py(spec, "spec")?,
            aggregation_source: AggregationSource::from_py(
                aggregation_source,
                "aggregation_source",
            )?,
            composite: None,
        }))
    }
    #[staticmethod]
    fn from_str(value: &str) -> PyResult<Self> {
        Ok(Self(check(BarType::parse(value), "BarType")?))
    }
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.0.instrument_id)
    }
    #[getter]
    fn spec(&self) -> PyBarSpecification {
        PyBarSpecification(self.0.spec)
    }
    #[getter]
    fn aggregation_source<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.aggregation_source.to_py(py)
    }
    fn is_externally_aggregated(&self) -> bool {
        self.0.aggregation_source == AggregationSource::External
    }
    fn is_internally_aggregated(&self) -> bool {
        self.0.aggregation_source == AggregationSource::Internal
    }
    fn is_composite(&self) -> bool {
        self.0.composite.is_some()
    }
    fn __str__(&self) -> String {
        self.0.to_string()
    }
    fn __repr__(&self) -> String {
        format!("BarType('{}')", self.0)
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        eq_only(py, op, other.cast::<Self>().ok().map(|o| o.get().0 == self.0))
    }
    fn __hash__(&self) -> isize {
        crc_hash(&self.0.to_string())
    }
    fn __reduce__<'py>(slf: &Bound<'py, Self>) -> PyResult<(Bound<'py, PyAny>, (String,))> {
        Ok((slf.get_type().getattr("from_str")?, (slf.get().0.to_string(),)))
    }
}

/// One order or price level of an order book.
#[pyclass(frozen, name = "BookOrder", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyBookOrder(pub BookOrder);

#[pymethods]
impl PyBookOrder {
    #[new]
    #[pyo3(signature = (side, price, size, order_id = 0))]
    fn new(
        side: &Bound<'_, PyAny>,
        price: &Bound<'_, PyAny>,
        size: &Bound<'_, PyAny>,
        order_id: u64,
    ) -> PyResult<Self> {
        Ok(Self(BookOrder {
            side: <Option<OrderSide>>::from_py(side, "side")?,
            price: Price::from_py(price, "price")?,
            size: Quantity::from_py(size, "size")?,
            order_id,
        }))
    }
    #[getter]
    fn side<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.side.to_py(py)
    }
    #[getter]
    fn price(&self) -> PyPrice {
        PyPrice(self.0.price)
    }
    #[getter]
    fn size(&self) -> PyQuantity {
        PyQuantity(self.0.size)
    }
    #[getter]
    fn order_id(&self) -> u64 {
        self.0.order_id
    }
    #[classattr]
    fn fields() -> (&'static str, &'static str, &'static str, &'static str) {
        ("side", "price", "size", "order_id")
    }
    fn to_dict<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, pyo3::types::PyDict>> {
        crate::common::dict_of(
            py,
            &[
                ("side", self.0.side.to_py(py)?),
                ("price", self.0.price.to_py(py)?),
                ("size", self.0.size.to_py(py)?),
                ("order_id", self.0.order_id.to_py(py)?),
            ],
        )
    }
    fn __repr__(&self, py: Python<'_>) -> PyResult<String> {
        Ok(format!(
            "BookOrder(side={}, price={}, size={}, order_id={})",
            self.0.side.to_py(py)?.repr()?,
            PyPrice(self.0.price).__repr__(),
            PyQuantity(self.0.size).__repr__(),
            self.0.order_id
        ))
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        eq_only(py, op, other.cast::<Self>().ok().map(|o| o.get().0 == self.0))
    }
    fn __hash__(&self) -> isize {
        (self.0.price.raw() ^ (self.0.size.raw() as i64) ^ (self.0.order_id as i64)) as isize
    }
    fn __reduce__<'py>(
        slf: &Bound<'py, Self>,
    ) -> PyResult<(Bound<'py, PyAny>, (Bound<'py, PyAny>, PyPrice, PyQuantity, u64))> {
        let o = slf.get().0;
        Ok((
            slf.get_type().into_any(),
            (o.side.to_py(slf.py())?, PyPrice(o.price), PyQuantity(o.size), o.order_id),
        ))
    }
}

/// `"{node_tag}-{epoch}-{seq}"` client order ids (section 8.4): no wall clock, decodable.
#[pyclass(name = "ClientOrderIdGenerator", module = "jarvis._core.model")]
#[derive(Debug)]
pub struct PyClientOrderIdGenerator(pub Generator);

#[pymethods]
impl PyClientOrderIdGenerator {
    #[new]
    fn new(node_tag: &str, epoch: u64) -> PyResult<Self> {
        Ok(Self(check(Generator::new(node_tag, epoch), "ClientOrderIdGenerator")?))
    }
    fn next(&mut self) -> PyResult<PyClientOrderId> {
        Ok(PyClientOrderId(check(self.0.next_id(), "ClientOrderIdGenerator.next")?))
    }
    /// `(node_tag, epoch, seq)` of a jarvis client order id.
    #[staticmethod]
    fn decode(client_order_id: &Bound<'_, PyAny>) -> PyResult<(String, u64, u64)> {
        let id = ClientOrderId::from_py(client_order_id, "client_order_id")?;
        let d = check(model::client_order_id::decode(&id), "ClientOrderIdGenerator.decode")?;
        Ok((d.node_tag.to_string(), d.epoch, d.seq))
    }
}

pub(crate) fn add_classes(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyCurrency>()?;
    m.add_class::<PyPrice>()?;
    m.add_class::<PyQuantity>()?;
    m.add_class::<PyMoney>()?;
    m.add_class::<PySymbol>()?;
    m.add_class::<PyVenue>()?;
    m.add_class::<PyTraderId>()?;
    m.add_class::<PyStrategyId>()?;
    m.add_class::<PyAccountId>()?;
    m.add_class::<PyClientOrderId>()?;
    m.add_class::<PyVenueOrderId>()?;
    m.add_class::<PyClientId>()?;
    m.add_class::<PyComponentId>()?;
    m.add_class::<PyExecAlgorithmId>()?;
    m.add_class::<PyOrderListId>()?;
    m.add_class::<PyTradeId>()?;
    m.add_class::<PyPositionId>()?;
    m.add_class::<PyInstrumentId>()?;
    m.add_class::<PyUuid4>()?;
    m.add_class::<PyBarSpecification>()?;
    m.add_class::<PyBarType>()?;
    m.add_class::<PyBookOrder>()?;
    m.add_class::<PyClientOrderIdGenerator>()?;
    Ok(())
}
