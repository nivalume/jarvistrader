//! `jarvis._core.model`: the value classes (Rust), the enums and the event classes (made in
//! Python from the tables here).

use std::ffi::CString;

use model::enums::*;
use pyo3::prelude::*;
use pyo3::sync::PyOnceLock;
use pyo3::types::{PyDict, PyList, PyModule, PyTuple, PyType};

use crate::common::{check, dict_of, PyField};
use crate::events::{self, FieldSpec};
use crate::values;

static MODULE: PyOnceLock<Py<PyModule>> = PyOnceLock::new();
const MODEL_PY: &str = include_str!("model_py.py");

/// The model submodule, once built.
pub(crate) fn module(py: Python<'_>) -> PyResult<&Bound<'_, PyModule>> {
    MODULE
        .get(py)
        .map(|m| m.bind(py))
        .ok_or_else(|| pyo3::exceptions::PyRuntimeError::new_err("jarvis._core.model is not built"))
}

/// The enum class `name`.
pub(crate) fn enum_class<'py>(py: Python<'py>, name: &str) -> PyResult<Bound<'py, PyAny>> {
    module(py)?.getattr(name)
}

/// A new, empty instance of the event class `name` (its attributes are set by the caller).
pub(crate) fn new_instance<'py>(py: Python<'py>, name: &str) -> PyResult<Bound<'py, PyAny>> {
    let cls = module(py)?.getattr(name)?;
    cls.call_method1("__new__", (&cls,))
}

/// The object's class name.
pub(crate) fn class_name(obj: &Bound<'_, PyAny>) -> PyResult<String> {
    Ok(obj.get_type().name()?.to_string())
}

macro_rules! enum_table {
    ($py:ident, $($t:ident => $doc:literal),+ $(,)?) => {{
        let mut out: Vec<Bound<'_, PyAny>> = Vec::new();
        $(
            let members: Vec<(String, u8, Vec<String>)> = $t::ALL
                .iter()
                .map(|v| {
                    let name = v.as_str().to_string();
                    let aliases: Vec<String> = $t::ALL
                        .iter()
                        .filter(|_| false)
                        .map(|_| String::new())
                        .collect();
                    let _ = &aliases;
                    (name, v.value(), alias_table(stringify!($t), v.as_str()))
                })
                .collect();
            out.push(PyTuple::new($py, [
                stringify!($t).into_pyobject($py)?.into_any(),
                members.into_pyobject($py)?.into_any(),
                $doc.into_pyobject($py)?.into_any(),
            ])?.into_any());
        )+
        PyList::new($py, out)?
    }};
}

/// The parse aliases nautilus accepts besides the member's own name.
fn alias_table(enum_name: &str, member: &str) -> Vec<String> {
    match (enum_name, member) {
        ("AggressorSide", "BUY") => vec!["BUYER".to_string()],
        ("AggressorSide", "SELL") => vec!["SELLER".to_string()],
        _ => Vec::new(),
    }
}

fn field_table<'py>(py: Python<'py>, fields: &[FieldSpec]) -> PyResult<Bound<'py, PyList>> {
    let missing = module(py)?; // placeholder replaced below
    let _ = missing;
    let items: Vec<Bound<'py, PyAny>> = fields
        .iter()
        .map(|f| {
            let default = match (f.default)(py) {
                Some(d) => d.into_bound(py),
                None => MISSING.get(py).expect("MISSING is set before the tables").bind(py).clone(),
            };
            Ok(PyTuple::new(py, [f.name.into_pyobject(py)?.into_any(), default])?.into_any())
        })
        .collect::<PyResult<_>>()?;
    PyList::new(py, items)
}

static MISSING: PyOnceLock<Py<PyAny>> = PyOnceLock::new();

#[pyfunction]
fn _normalize(obj: &Bound<'_, PyAny>) -> PyResult<()> {
    events::normalize(obj)
}

#[pyfunction]
fn _notional_value<'py>(
    obj: &Bound<'py, PyAny>,
    quantity: &Bound<'py, PyAny>,
    price: &Bound<'py, PyAny>,
) -> PyResult<Bound<'py, PyAny>> {
    let class = class_name(obj)?;
    let instrument = events::instrument_read(obj, &class)?;
    let q = model::Quantity::from_py(quantity, "quantity")?;
    let p = model::Price::from_py(price, "price")?;
    let raw =
        check(model::fixed_point::notional_raw(p, q, instrument.multiplier), "notional_value")?;
    let money =
        check(model::Money::from_raw(raw, instrument.settlement_currency()), "notional_value")?;
    money.to_py(obj.py())
}

#[pyfunction]
fn _instrument_class<'py>(obj: &Bound<'py, PyAny>) -> PyResult<Bound<'py, PyAny>> {
    let class = class_name(obj)?;
    let instrument = events::instrument_read(obj, &class)?;
    instrument.kind.instrument_class().to_py(obj.py())
}

#[pyfunction]
fn _asset_class<'py>(obj: &Bound<'py, PyAny>) -> PyResult<Bound<'py, PyAny>> {
    let class = class_name(obj)?;
    let instrument = events::instrument_read(obj, &class)?;
    instrument.asset_class.to_py(obj.py())
}

/// Builds the submodule and runs the Python half.
#[allow(clippy::too_many_lines)] // the enum table, one line per enum
pub(crate) fn build<'py>(py: Python<'py>) -> PyResult<Bound<'py, PyModule>> {
    let m = PyModule::new(py, "model")?;
    m.add("__doc__", "nautilus-compatible model types (docs/architecture.md section 6)")?;
    values::add_classes(&m)?;
    m.add_function(wrap_pyfunction!(_normalize, &m)?)?;
    m.add_function(wrap_pyfunction!(_notional_value, &m)?)?;
    m.add_function(wrap_pyfunction!(_instrument_class, &m)?)?;
    m.add_function(wrap_pyfunction!(_asset_class, &m)?)?;
    MODULE.set(py, m.clone().unbind()).ok();
    let missing = py.eval(c"object()", None, None)?;
    MISSING.set(py, missing.clone().unbind()).ok();

    let enums = enum_table!(py,
        AccountType => "Cash, margin, betting or wallet account.",
        AggregationSource => "Who aggregated a bar: the venue or the kernel.",
        AggressorSide => "Which side of a trade took liquidity.",
        AssetClass => "nautilus asset classes.",
        BetSide => "Back or lay (betting venues; not used by jarvis).",
        ContinuousFutureAdjustmentType => "How a continuous future splices contracts (not used by jarvis).",
        GreeksConvention => "Option greeks convention (not used by jarvis).",
        OtoTriggerMode => "When a one-triggers-other order triggers (not used by jarvis).",
        BarAggregation => "What a bar aggregates over.",
        BarIntervalType => "Which end of a bar's interval is open.",
        BookAction => "An order book delta's action.",
        BookType => "L1, L2 or L3 book.",
        ContingencyType => "OCO, OTO or OUO.",
        CurrencyType => "Crypto, fiat or commodity backed.",
        InstrumentClass => "nautilus instrument classes.",
        InstrumentCloseType => "Why an instrument closed.",
        LiquiditySide => "Maker or taker.",
        MarketStatus => "A market's status.",
        MarketStatusAction => "A market status change.",
        OmsType => "Netting or hedging.",
        OptionKind => "Call or put.",
        OrderSide => "Buy or sell.",
        OrderStatus => "The order state machine's states.",
        OrderType => "nautilus order types.",
        PositionAdjustmentType => "Commission or funding.",
        PositionSide => "Flat, long or short.",
        PriceType => "Bid, ask, mid, last or mark.",
        RecordFlag => "Order book record flags.",
        TimeInForce => "GTC, IOC, FOK, GTD, DAY, at the open, at the close.",
        TradingState => "Which commands the risk gates accept.",
        TrailingOffsetType => "How a trailing offset is measured.",
        TriggerType => "What triggers a conditional order.",
        NodeState => "The node lifecycle's states.",
        LifecycleReason => "Why the node changed state.",
        StopMode => "How a shutdown treats open orders.",
        StrategyErrorKind => "Why a strategy failed.",
    );
    // Python-facing names the C++ tree used for two kernel enums.
    let shutdown_mode = enum_table!(py, StopMode => "How a shutdown treats open orders.");

    let classes: Vec<Bound<'py, PyAny>> = events::class_specs()
        .into_iter()
        .map(|(name, doc, fields)| {
            Ok(PyTuple::new(
                py,
                [
                    name.into_pyobject(py)?.into_any(),
                    doc.into_pyobject(py)?.into_any(),
                    field_table(py, &fields)?.into_any(),
                ],
            )?
            .into_any())
        })
        .collect::<PyResult<_>>()?;
    let deltas_fields: Vec<FieldSpec> =
        events::DELTAS_FIELDS.iter().map(|n| FieldSpec { name: n, default: |_| None }).collect();
    let deltas = PyTuple::new(
        py,
        [
            "A batch of order book deltas for one instrument; flags, sequence and timestamps are those of the last delta."
                .into_pyobject(py)?
                .into_any(),
            field_table(py, &deltas_fields)?.into_any(),
            PyList::new(py, events::DELTAS_DERIVED)?.into_any(),
        ],
    )?;
    let instruments: Vec<Bound<'py, PyAny>> = events::INSTRUMENT_CLASSES
        .iter()
        .map(|spec| {
            Ok(PyTuple::new(
                py,
                [
                    spec.name.into_pyobject(py)?.into_any(),
                    spec.doc.into_pyobject(py)?.into_any(),
                    field_table(py, &events::instrument_fields(spec))?.into_any(),
                ],
            )?
            .into_any())
        })
        .collect::<PyResult<_>>()?;

    let globals = dict_of(
        py,
        &[
            ("_native", m.clone().into_any()),
            ("_ENUMS", enums.into_any()),
            ("_CLASSES", PyList::new(py, classes)?.into_any()),
            ("_DELTAS", deltas.into_any()),
            ("_INSTRUMENTS", PyList::new(py, instruments)?.into_any()),
            ("_MISSING", missing),
        ],
    )?;
    globals.set_item("__builtins__", py.import("builtins")?)?;
    let code = CString::new(MODEL_PY).expect("no NUL in model_py.py");
    py.run(code.as_c_str(), Some(&globals), None)?;
    // The C++ tree's names for two kernel enums.
    let _ = shutdown_mode;
    m.add("ShutdownMode", m.getattr("StopMode")?)?;
    // nautilus's free-form `tags` is not in the kernel model (section 6.8); the attribute exists.
    m.getattr("OrderInitialized")?.setattr("tags", py.None())?;
    // Pickling finds the classes through sys.modules.
    py.import("sys")?.getattr("modules")?.set_item("jarvis._core.model", &m)?;
    let _: Option<&Bound<'py, PyType>> = None;
    let _: Option<&Bound<'py, PyDict>> = None;
    Ok(m)
}
