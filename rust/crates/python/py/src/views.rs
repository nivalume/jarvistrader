//! Python classes shared by the host and the node: cadences, the `DataKind` and `FeatureKind`
//! enums, feature declarations, order intents and the copies of kernel state a strategy reads
//! (`OrderView`, `PositionView`, `ExposureView`).

use data::subscription::{Cadence, DataKind};
use data::FeatureKind;
use kernel_core::{Status, UnixNanos};
use model::decimal::format_raw;
use model::enums::{OrderSide, OrderType, TimeInForce};
use model::{InstrumentId, Money, Price, Quantity};
use pyo3::basic::CompareOp;
use pyo3::exceptions::PyValueError;
use pyo3::prelude::*;
use pyo3::sync::PyOnceLock;
use pyo3::types::{PyDict, PyModule};
use strategy::{ExposureView, OrderIntent, OrderView, PositionView};

use crate::common::{check, python_decimal, type_error, PyField};
use crate::values::{eq_only, PyInstrumentId};

pub(crate) const MODULE: &str = "jarvis._core.node";

// ---- enums (`enum.Enum` classes made at import) ----------------------------------------------

/// The Python `DataKind` and `FeatureKind` classes, once the module is built.
pub(crate) static DATA_KIND: PyOnceLock<Py<PyAny>> = PyOnceLock::new();
pub(crate) static FEATURE_KIND: PyOnceLock<Py<PyAny>> = PyOnceLock::new();

/// Member names and values of `DataKind`, as the C++ binding orders them.
pub(crate) const DATA_KINDS: [(&str, u8); 12] = [
    ("TRADE", 0),
    ("QUOTE", 1),
    ("BOOK_DELTAS", 2),
    ("BOOK", 3),
    ("BAR", 4),
    ("MARK_PRICE", 5),
    ("INDEX_PRICE", 6),
    ("FUNDING_RATE", 7),
    ("STATUS", 8),
    ("CLOSE", 9),
    ("LIQUIDATION", 10),
    ("FEATURE", 11),
];
pub(crate) const FEATURE_KINDS: [(&str, u8); 5] =
    [("EMA", 0), ("VWAP", 1), ("IMBALANCE", 2), ("MICROPRICE", 3), ("REALIZED_VOL", 4)];

/// An `enum.Enum` class with `members`, in this module.
pub(crate) fn make_enum<'py>(
    py: Python<'py>,
    name: &str,
    members: &[(&str, u8)],
) -> PyResult<Bound<'py, PyAny>> {
    let values = PyDict::new(py);
    for (member, value) in members {
        values.set_item(member, value)?;
    }
    let kwargs = PyDict::new(py);
    kwargs.set_item("module", MODULE)?;
    py.import("enum")?.getattr("Enum")?.call((name, values), Some(&kwargs))
}

fn enum_class<'py>(
    py: Python<'py>,
    lock: &'static PyOnceLock<Py<PyAny>>,
) -> PyResult<Bound<'py, PyAny>> {
    lock.get(py)
        .map(|c| c.bind(py).clone())
        .ok_or_else(|| PyValueError::new_err("jarvis._core.node is not initialised"))
}

/// The member of the enum in `lock` with `value`.
pub(crate) fn enum_member<'py>(
    py: Python<'py>,
    lock: &'static PyOnceLock<Py<PyAny>>,
    value: u8,
) -> PyResult<Bound<'py, PyAny>> {
    enum_class(py, lock)?.call1((value,))
}

/// The value of a member of the enum in `lock`.
pub(crate) fn enum_value(
    obj: &Bound<'_, PyAny>,
    lock: &'static PyOnceLock<Py<PyAny>>,
    what: &str,
) -> PyResult<u8> {
    let class = enum_class(obj.py(), lock)?;
    if !obj.is_instance(&class)? {
        let name = class.getattr("__name__")?.extract::<String>()?;
        return Err(type_error(what, &format!("a {name}"), obj));
    }
    obj.getattr("value")?.extract::<u8>()
}

/// The kernel's kind for a `DataKind` member; `BOOK_DELTAS` shares the book column.
pub(crate) fn data_kind_of(obj: &Bound<'_, PyAny>, what: &str) -> PyResult<DataKind> {
    Ok(match enum_value(obj, &DATA_KIND, what)? {
        0 => DataKind::Trade,
        1 => DataKind::Quote,
        2 | 3 => DataKind::Book,
        4 => DataKind::Bar,
        5 => DataKind::MarkPrice,
        6 => DataKind::IndexPrice,
        7 => DataKind::FundingRate,
        8 => DataKind::InstrumentStatus,
        9 => DataKind::InstrumentClose,
        10 => DataKind::Liquidation,
        _ => DataKind::Feature,
    })
}

// ---- Cadence ----------------------------------------------------------------------------------

/// When a subscriber sees updates.
#[pyclass(frozen, name = "Cadence", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyCadence(pub(crate) Cadence);

#[pymethods]
impl PyCadence {
    #[classattr]
    #[allow(non_snake_case)]
    fn EVERY() -> Self {
        Self(Cadence::Every)
    }
    #[classattr]
    #[allow(non_snake_case)]
    fn CONFLATED() -> Self {
        Self(Cadence::Conflated)
    }
    #[classattr]
    #[allow(non_snake_case)]
    fn ON_BATCH() -> Self {
        Self(Cadence::OnBatch)
    }
    #[staticmethod]
    fn every() -> Self {
        Self(Cadence::Every)
    }
    #[staticmethod]
    fn conflated() -> Self {
        Self(Cadence::Conflated)
    }
    #[staticmethod]
    fn on_batch() -> Self {
        Self(Cadence::OnBatch)
    }
    /// The first update in each period, periods aligned to the epoch.
    #[staticmethod]
    fn sampled_ns(period_ns: u64) -> PyResult<Self> {
        Ok(Self(check(Cadence::sampled_ns(period_ns), "Cadence.sampled_ns")?))
    }
    #[staticmethod]
    fn sampled_ms(period_ms: u64) -> PyResult<Self> {
        Ok(Self(check(Cadence::sampled_ms(period_ms), "Cadence.sampled_ms")?))
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        eq_only(py, op, other.cast::<Self>().ok().map(|o| o.get().0 == self.0))
    }
    fn __hash__(&self) -> u64 {
        match self.0 {
            Cadence::Every => 1,
            Cadence::Conflated => 2,
            Cadence::OnBatch => 3,
            Cadence::Sampled { period_ns } => period_ns.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 4,
        }
    }
    fn __repr__(&self) -> String {
        match self.0 {
            Cadence::Every => "Cadence.EVERY".to_string(),
            Cadence::Conflated => "Cadence.CONFLATED".to_string(),
            Cadence::OnBatch => "Cadence.ON_BATCH".to_string(),
            Cadence::Sampled { period_ns } => format!("Cadence.sampled_ns({period_ns})"),
        }
    }
}

/// `EVERY` when the argument was left out.
pub(crate) fn cadence_of(cadence: Option<&Bound<'_, PyCadence>>) -> Cadence {
    cadence.map_or(Cadence::Every, |c| c.get().0)
}

// ---- FeatureSpec ------------------------------------------------------------------------------

/// A kernel feature declaration: the kind, the instrument and a window where the kind has one.
#[pyclass(frozen, name = "FeatureSpec", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyFeatureSpec {
    kind: u8,
    pub(crate) instrument_id: InstrumentId,
    window: u32,
}

impl PyFeatureSpec {
    pub(crate) fn feature_kind(&self) -> PyResult<FeatureKind> {
        let window = self.window;
        Ok(match self.kind {
            0 => FeatureKind::Ema { period: window },
            1 => FeatureKind::Vwap { window },
            2 => FeatureKind::Imbalance,
            3 => FeatureKind::Microprice,
            4 => FeatureKind::RealizedVol { window },
            other => {
                return Err(PyValueError::new_err(format!("FeatureSpec: unknown kind {other}")))
            }
        })
    }
}

#[pymethods]
impl PyFeatureSpec {
    #[new]
    #[pyo3(signature = (kind, instrument_id, window = 0))]
    fn new(
        kind: &Bound<'_, PyAny>,
        instrument_id: &Bound<'_, PyAny>,
        window: u32,
    ) -> PyResult<Self> {
        Ok(Self {
            kind: enum_value(kind, &FEATURE_KIND, "kind")?,
            instrument_id: InstrumentId::from_py(instrument_id, "instrument_id")?,
            window,
        })
    }
    #[getter]
    fn kind<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        enum_member(py, &FEATURE_KIND, self.kind)
    }
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.instrument_id)
    }
    #[getter]
    fn window(&self) -> u32 {
        self.window
    }
    fn __richcmp__<'py>(
        &self,
        other: &Bound<'py, PyAny>,
        op: CompareOp,
        py: Python<'py>,
    ) -> Bound<'py, PyAny> {
        let equal = other.cast::<Self>().ok().map(|o| {
            let o = o.get();
            o.kind == self.kind && o.instrument_id == self.instrument_id && o.window == self.window
        });
        eq_only(py, op, equal)
    }
    fn __hash__(&self) -> u64 {
        let mut h = kernel_core::crc32c::crc32c(self.instrument_id.to_string().as_bytes());
        h ^= u32::from(self.kind).wrapping_mul(0x9E37_79B9);
        (u64::from(h) << 32) | u64::from(self.window)
    }
    fn __repr__(&self, py: Python<'_>) -> PyResult<String> {
        let kind =
            enum_member(py, &FEATURE_KIND, self.kind)?.getattr("name")?.extract::<String>()?;
        Ok(format!("FeatureSpec(FeatureKind.{kind}, '{}', {})", self.instrument_id, self.window))
    }
}

// ---- decimals at a precision --------------------------------------------------------------------

/// A raw 1e9 value as `decimal.Decimal` with `precision` places (the instrument's).
pub(crate) fn decimal_at(py: Python<'_>, raw: i64, precision: u8) -> PyResult<Bound<'_, PyAny>> {
    let mut buf = [0u8; 64];
    let n = format_raw(i128::from(raw), precision.min(9), &mut buf);
    python_decimal(py, core::str::from_utf8(&buf[..n]).unwrap_or("0"))
}

fn optional<'py, T: PyField>(py: Python<'py>, value: Option<&T>) -> PyResult<Bound<'py, PyAny>> {
    match value {
        Some(v) => v.to_py(py),
        None => Ok(py.None().into_bound(py)),
    }
}

// ---- OrderIntent --------------------------------------------------------------------------------

/// What a strategy asks for; editable until `submit`.
#[pyclass(name = "OrderIntent", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyOrderIntent(pub(crate) OrderIntent);

macro_rules! intent_fields {
    ($( $get:ident / $set:ident : $field:ident : $t:ty ),* $(,)?) => {
        #[pymethods]
        impl PyOrderIntent {
            #[new]
            fn new() -> Self {
                Self(OrderIntent::market(InstrumentId::default(), OrderSide::Buy, Quantity::default()))
            }
            fn __repr__(&self) -> String {
                let i = &self.0;
                let price = i.price.map_or_else(|| "None".to_string(), |p| p.to_string());
                format!(
                    "OrderIntent({} {} {} {} price={price} {} post_only={} reduce_only={})",
                    i.instrument_id,
                    i.side.as_str(),
                    i.order_type.as_str(),
                    i.quantity,
                    i.time_in_force.as_str(),
                    i.post_only,
                    i.reduce_only
                )
            }
            $(
                #[getter($field)]
                fn $get<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
                    self.0.$field.to_py(py)
                }
                #[setter($field)]
                fn $set(&mut self, value: &Bound<'_, PyAny>) -> PyResult<()> {
                    self.0.$field = <$t>::from_py(value, stringify!($field))?;
                    Ok(())
                }
            )*
        }
    };
}
intent_fields!(
    get_instrument_id / set_instrument_id: instrument_id: InstrumentId,
    get_side / set_side: side: OrderSide,
    get_order_type / set_order_type: order_type: OrderType,
    get_quantity / set_quantity: quantity: Quantity,
    get_price / set_price: price: Option<Price>,
    get_time_in_force / set_time_in_force: time_in_force: TimeInForce,
    get_post_only / set_post_only: post_only: bool,
    get_reduce_only / set_reduce_only: reduce_only: bool,
    get_expire_time / set_expire_time: expire_time: Option<UnixNanos>,
);

// ---- OrderView ----------------------------------------------------------------------------------

/// A copy of one order: identity, terms and current state.
#[pyclass(frozen, name = "OrderView", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyOrderView(pub(crate) OrderView);

#[pymethods]
impl PyOrderView {
    #[getter]
    fn client_order_id<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.client_order_id.to_py(py)
    }
    #[getter]
    fn venue_order_id<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        optional(py, self.0.venue_order_id.as_ref())
    }
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.0.instrument_id)
    }
    #[getter]
    fn side<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.side.to_py(py)
    }
    #[getter]
    fn order_type<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.order_type.to_py(py)
    }
    #[getter]
    fn time_in_force<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.time_in_force.to_py(py)
    }
    #[getter]
    fn post_only(&self) -> bool {
        self.0.post_only
    }
    #[getter]
    fn reduce_only(&self) -> bool {
        self.0.reduce_only
    }
    #[getter]
    fn price<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        optional(py, self.0.price.as_ref())
    }
    #[getter]
    fn status<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.status.to_py(py)
    }
    #[getter]
    fn quantity<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.quantity.to_py(py)
    }
    #[getter]
    fn filled_qty<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.filled.to_py(py)
    }
    #[getter]
    fn leaves_qty<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.0.leaves.to_py(py)
    }
    #[getter]
    fn avg_px<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        optional(py, self.0.avg_px.as_ref())
    }
    /// Parent orders belong to the execution algorithms, which this tree does not have yet.
    #[getter]
    #[allow(clippy::unused_self)] // a property of every view
    fn parent_id<'py>(&self, py: Python<'py>) -> Bound<'py, PyAny> {
        py.None().into_bound(py)
    }
    #[getter]
    fn ts_init(&self) -> u64 {
        self.0.ts_init.value()
    }
    fn __repr__(&self) -> String {
        format!("OrderView({} {})", self.0.client_order_id, self.0.status.as_str())
    }
}

// ---- PositionView -------------------------------------------------------------------------------

/// A strategy's position in one instrument; amounts in the settlement currency.
#[pyclass(frozen, name = "PositionView", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyPositionView {
    pub(crate) view: PositionView,
    /// The instrument's size precision, for `signed_qty`.
    pub(crate) size_precision: u8,
}

#[pymethods]
impl PyPositionView {
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.view.instrument_id)
    }
    #[getter]
    fn position_id<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.position_id.to_py(py)
    }
    #[getter]
    fn side<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.side.to_py(py)
    }
    /// Signed, positive long, as a `Decimal` at the instrument's size precision.
    #[getter]
    fn signed_qty<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        decimal_at(py, self.view.signed_raw, self.size_precision)
    }
    #[getter]
    fn quantity<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.quantity.to_py(py)
    }
    #[getter]
    fn avg_px_open<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        optional(py, self.view.avg_px_open.as_ref())
    }
    #[getter]
    fn realized_pnl<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.realized_pnl.to_py(py)
    }
    #[getter]
    fn unrealized_pnl<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.unrealized_pnl.to_py(py)
    }
    #[getter]
    fn commission<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.commission.to_py(py)
    }
    #[getter]
    fn funding<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.funding.to_py(py)
    }
    #[getter]
    fn total_pnl<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.total_pnl.to_py(py)
    }
    #[getter]
    fn ts_opened(&self) -> u64 {
        self.view.ts_opened.value()
    }
    fn __repr__(&self) -> String {
        format!(
            "PositionView({} {} {})",
            self.view.position_id,
            self.view.side.as_str(),
            self.view.quantity
        )
    }
}

// ---- ExposureView -------------------------------------------------------------------------------

/// `open_exposure()` of one instrument: the account's position and its open orders.
#[pyclass(frozen, name = "ExposureView", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyExposureView {
    pub(crate) view: ExposureView,
    pub(crate) size_precision: u8,
}

#[pymethods]
impl PyExposureView {
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.view.instrument_id)
    }
    #[getter]
    fn position<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        decimal_at(py, self.view.position_raw, self.size_precision)
    }
    #[getter]
    fn open_buy<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.open_buy.to_py(py)
    }
    #[getter]
    fn open_sell<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.view.open_sell.to_py(py)
    }
    #[getter]
    fn max_long<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        decimal_at(py, self.view.max_long_raw, self.size_precision)
    }
    #[getter]
    fn max_short<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        decimal_at(py, self.view.max_short_raw, self.size_precision)
    }
    #[getter]
    fn notional<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        optional::<Money>(py, self.view.notional.as_ref())
    }
    fn __repr__(&self) -> String {
        format!("ExposureView({} position_raw={})", self.view.instrument_id, self.view.position_raw)
    }
}

/// A parent order of an execution algorithm. Execution algorithms arrive with docs/rust-plan.md
/// R3's second step; until then no instance exists (`Context.parent` returns None).
#[pyclass(frozen, name = "ParentView", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyParentView {
    parent_id: model::ClientOrderId,
}

#[pymethods]
impl PyParentView {
    #[getter]
    fn parent_id<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.parent_id.to_py(py)
    }
    fn __repr__(&self) -> String {
        format!("ParentView({})", self.parent_id)
    }
}

/// `Status::InvalidState` as `False`, success as `True`; other failures raise.
pub(crate) fn accepted(result: kernel_core::Result<()>, what: &str) -> PyResult<bool> {
    match result {
        Ok(()) => Ok(true),
        Err(Status::InvalidState) => Ok(false),
        Err(s) => Err(crate::common::status_error(what, s)),
    }
}

pub(crate) fn add_classes(py: Python<'_>, m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyCadence>()?;
    m.add_class::<PyFeatureSpec>()?;
    m.add_class::<PyOrderIntent>()?;
    m.add_class::<PyOrderView>()?;
    m.add_class::<PyParentView>()?;
    m.add_class::<PyPositionView>()?;
    m.add_class::<PyExposureView>()?;
    let data_kind = make_enum(py, "DataKind", &DATA_KINDS)?;
    let feature_kind = make_enum(py, "FeatureKind", &FEATURE_KINDS)?;
    m.add("DataKind", &data_kind)?;
    m.add("FeatureKind", &feature_kind)?;
    let _ = DATA_KIND.set(py, data_kind.unbind());
    let _ = FEATURE_KIND.set(py, feature_kind.unbind());
    Ok(())
}
