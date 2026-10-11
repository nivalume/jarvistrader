//! The Python strategy host (docs/architecture.md sections 4.5 and 7.3-7.7): a
//! [`PyStrategyHost`] adapts one Python strategy object to the kernel's [`Strategy`] trait, so
//! Python strategies and Rust strategies share one engine. Callbacks a strategy does not define
//! are skipped without touching Python. An exception becomes a recorded `StrategyError`; the
//! node's error policy decides what follows.
//!
//! A Python `Context` object belongs to one host and is handed to every callback. It holds a
//! pointer to the kernel context for the duration of that callback and nothing afterwards: a
//! context kept by the strategy raises `TypeError` when used. That pointer is the crate's one
//! `unsafe` (see [`PyContext::with`]).

use core::cell::Cell;
use core::ptr::NonNull;
use std::rc::Rc;

use data::book::{BookLevel, BookView};
use data::{FeatureId, FeatureValue};
use kernel_core::clock::TimerKey;
use kernel_core::state::{StateReader, StateWriter};
use kernel_core::{DurationNanos, Result as KResult, Status, UnixNanos};
use model::bar::{Bar, BarType};
use model::data::{
    FundingRateUpdate, IndexPriceUpdate, InstrumentClose, InstrumentStatus, LiquidationOrder,
    MarkPriceUpdate, OrderBookDeltas, QuoteTick, TradeTick,
};
use model::enums::{OrderSide, StrategyErrorKind, TimeInForce};
use model::event::{Event, StrategyError};
use model::order_events::OrderEvent;
use model::position_events::PositionEvent;
use model::{ClientOrderId, Currency, InstrumentId, Price, Quantity};
use pyo3::exceptions::{PyAttributeError, PyTypeError, PyValueError};
use pyo3::prelude::*;
use pyo3::sync::PyOnceLock;
use pyo3::types::{PyBytes, PyModule, PyTuple};
use strategy::{failure_hash, Context, OrderIntent, OrderView, Strategy};
use sys::Stopwatch;

use crate::common::{check, decimal9, raw9, PyField};
use crate::events::{event_to_py, order_event_to_py, position_event_to_py};
use crate::values::{PyClientOrderId, PyInstrumentId, PyPrice, PyQuantity};
use crate::views::{
    accepted, cadence_of, data_kind_of, PyCadence, PyExposureView, PyFeatureSpec, PyOrderIntent,
    PyOrderView, PyPositionView,
};

// ---- callbacks --------------------------------------------------------------------------------

/// The callbacks a Python strategy may define, by index.
pub(crate) const CALLBACKS: [&str; 22] = [
    "on_start",
    "on_stop",
    "on_trade",
    "on_quote",
    "on_book",
    "on_book_deltas",
    "on_bar",
    "on_mark_price",
    "on_index_price",
    "on_funding_rate",
    "on_instrument_status",
    "on_instrument_close",
    "on_liquidation",
    "on_feature",
    "on_trade_batch",
    "on_quote_batch",
    "on_timer",
    "on_error",
    "on_order_event",
    "on_position_event",
    "on_reconciled",
    "on_params_changed",
];
const ON_START: usize = 0;
const ON_STOP: usize = 1;
const ON_TRADE: usize = 2;
const ON_QUOTE: usize = 3;
const ON_BOOK: usize = 4;
const ON_BOOK_DELTAS: usize = 5;
const ON_BAR: usize = 6;
const ON_MARK_PRICE: usize = 7;
const ON_INDEX_PRICE: usize = 8;
const ON_FUNDING_RATE: usize = 9;
const ON_INSTRUMENT_STATUS: usize = 10;
const ON_INSTRUMENT_CLOSE: usize = 11;
const ON_LIQUIDATION: usize = 12;
const ON_FEATURE: usize = 13;
const ON_TRADE_BATCH: usize = 14;
const ON_QUOTE_BATCH: usize = 15;
const ON_TIMER: usize = 16;
const ON_ERROR: usize = 17;
const ON_ORDER_EVENT: usize = 18;
const ON_POSITION_EVENT: usize = 19;

/// Most of a traceback a strategy's failure prints.
const MAX_TRACEBACK: usize = 4096;
/// Levels per side a `BookView` copies.
const BOOK_DEPTH: usize = 64;

/// Callback timing of one host (wall clock; never part of the run's outputs).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct HostStats {
    pub(crate) calls: u64,
    pub(crate) total_ns: u64,
    pub(crate) max_ns: u64,
    pub(crate) overruns: u64,
    pub(crate) escapes: u64,
}

/// Shared with the node, which reads the stats after the run owns the host.
pub(crate) type SharedStats = Rc<Cell<HostStats>>;

#[derive(Clone, Copy, Debug)]
pub(crate) struct HostOptions {
    /// Sandbox and live only; never in a backtest or a replay.
    pub(crate) measure_overruns: bool,
    pub(crate) budget_ns: u64,
    pub(crate) overrun_limit: u64,
}

impl Default for HostOptions {
    fn default() -> Self {
        Self { measure_overruns: false, budget_ns: 2_000_000, overrun_limit: 50 }
    }
}

// ---- the context -------------------------------------------------------------------------------

fn expired(what: &str) -> PyErr {
    PyTypeError::new_err(format!(
        "{what} is only valid during the callback it was passed to; copy what you need to keep"
    ))
}

/// The kernel as a strategy sees it; valid only during the callback it was passed to.
#[pyclass(unsendable, name = "Context", module = "jarvis._core.node")]
pub(crate) struct PyContext {
    /// The running callback's kernel context, set by the host around the call.
    slot: Cell<Option<NonNull<Context<'static>>>>,
}

impl core::fmt::Debug for PyContext {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Context").field("active", &self.slot.get().is_some()).finish()
    }
}

impl PyContext {
    const fn new() -> Self {
        Self { slot: Cell::new(None) }
    }

    /// Runs `f` on the callback's kernel context; `TypeError` outside a callback.
    ///
    /// `f` must not call back into Python code that could reach this context again (argument
    /// conversions happen before, result conversions after), so the `&mut` it receives is the
    /// only live reference to the context while it runs.
    fn with<R>(&self, f: impl FnOnce(&mut Context<'_>) -> PyResult<R>) -> PyResult<R> {
        let Some(mut p) = self.slot.get() else { return Err(expired("Context")) };
        // SAFETY: the host stores the pointer in `invoke` while it holds `&mut Context` for the
        // duration of one Python call and clears it before that call returns to the kernel, so a
        // set slot always points at a live context; the host does not touch the context while
        // Python runs; Python runs one thread at a time under the GIL (`unsendable` keeps the
        // object on that thread); and `f` does not re-enter Python (see above), so no other
        // reference to the context exists while `f` runs.
        let ctx: &mut Context<'_> = unsafe { p.as_mut() };
        f(ctx)
    }
}

fn instrument_of(obj: &Bound<'_, PyAny>) -> PyResult<InstrumentId> {
    InstrumentId::from_py(obj, "instrument_id")
}

fn optional_of<T: PyField>(obj: Option<&Bound<'_, PyAny>>, what: &str) -> PyResult<Option<T>> {
    match obj {
        Some(o) if !o.is_none() => T::from_py(o, what).map(Some),
        _ => Ok(None),
    }
}

#[pymethods]
impl PyContext {
    /// The current input's time in nanoseconds: the only clock a strategy may read.
    fn now(&self) -> PyResult<u64> {
        self.with(|c| Ok(c.now().value()))
    }
    /// The current input's seq.
    fn seq(&self) -> PyResult<u64> {
        self.with(|c| Ok(c.seq()))
    }
    /// A deterministic 64-bit draw keyed by (seq, strategy, key).
    fn rng(&self, key: u32) -> PyResult<u64> {
        self.with(|c| Ok(c.rng(key)))
    }
    #[getter]
    fn strategy_index(&self) -> PyResult<u16> {
        self.with(|c| Ok(c.strategy().0))
    }

    /// A timer `timer_id` due at `deadline`, repeating every `period` nanoseconds when given.
    #[pyo3(signature = (timer_id, deadline, period = 0))]
    fn set_timer(&self, timer_id: u32, deadline: u64, period: u64) -> PyResult<()> {
        self.with(|c| {
            let deadline = UnixNanos::new(deadline);
            let r = if period == 0 {
                c.set_timer(timer_id, deadline)
            } else {
                c.set_periodic_timer(timer_id, deadline, DurationNanos::new(period))
            };
            check(r, "set_timer")
        })
    }
    fn cancel_timer(&self, timer_id: u32) -> PyResult<()> {
        self.with(|c| check(c.cancel_timer(timer_id), "cancel_timer"))
    }

    #[pyo3(signature = (instrument_id, cadence = None))]
    fn subscribe_trades(
        &self,
        instrument_id: &Bound<'_, PyAny>,
        cadence: Option<&Bound<'_, PyCadence>>,
    ) -> PyResult<()> {
        let id = instrument_of(instrument_id)?;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.subscribe_trades(&id, cadence), "subscribe_trades"))
    }
    #[pyo3(signature = (instrument_id, cadence = None))]
    fn subscribe_quotes(
        &self,
        instrument_id: &Bound<'_, PyAny>,
        cadence: Option<&Bound<'_, PyCadence>>,
    ) -> PyResult<()> {
        let id = instrument_of(instrument_id)?;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.subscribe_quotes(&id, cadence), "subscribe_quotes"))
    }
    /// The book after each update. The book's type follows the data (L1 from quotes, L2 from
    /// deltas); `book_type` is accepted for the C++ binding's signature.
    #[pyo3(signature = (instrument_id, cadence = None, book_type = None))]
    fn subscribe_book(
        &self,
        instrument_id: &Bound<'_, PyAny>,
        cadence: Option<&Bound<'_, PyCadence>>,
        book_type: Option<&Bound<'_, PyAny>>,
    ) -> PyResult<()> {
        let _ = book_type;
        let id = instrument_of(instrument_id)?;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.subscribe_book(&id, cadence), "subscribe_book"))
    }
    /// Every `OrderBookDeltas` of the instrument (`on_book_deltas`).
    fn subscribe_book_deltas(&self, instrument_id: &Bound<'_, PyAny>) -> PyResult<()> {
        let id = instrument_of(instrument_id)?;
        self.with(|c| {
            check(
                c.subscribe_book(&id, data::subscription::Cadence::Every),
                "subscribe_book_deltas",
            )
        })
    }
    #[pyo3(signature = (instrument_id, cadence = None))]
    fn subscribe_mark_price(
        &self,
        instrument_id: &Bound<'_, PyAny>,
        cadence: Option<&Bound<'_, PyCadence>>,
    ) -> PyResult<()> {
        let id = instrument_of(instrument_id)?;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.subscribe_mark_price(&id, cadence), "subscribe_mark_price"))
    }
    #[pyo3(signature = (instrument_id, cadence = None))]
    fn subscribe_funding(
        &self,
        instrument_id: &Bound<'_, PyAny>,
        cadence: Option<&Bound<'_, PyCadence>>,
    ) -> PyResult<()> {
        let id = instrument_of(instrument_id)?;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.subscribe_funding(&id, cadence), "subscribe_funding"))
    }
    #[pyo3(signature = (bar_type, cadence = None))]
    fn subscribe_bars(
        &self,
        bar_type: &Bound<'_, PyAny>,
        cadence: Option<&Bound<'_, PyCadence>>,
    ) -> PyResult<()> {
        let bar_type = BarType::from_py(bar_type, "bar_type")?;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.subscribe_bars(&bar_type, cadence), "subscribe_bars"))
    }
    fn unsubscribe(
        &self,
        kind: &Bound<'_, PyAny>,
        instrument_id: &Bound<'_, PyAny>,
    ) -> PyResult<()> {
        let kind = data_kind_of(kind, "kind")?;
        let id = instrument_of(instrument_id)?;
        self.with(|c| check(c.unsubscribe(kind, &id), "unsubscribe"))
    }
    fn unsubscribe_bars(&self, bar_type: &Bound<'_, PyAny>) -> PyResult<()> {
        let bar_type = BarType::from_py(bar_type, "bar_type")?;
        self.with(|c| check(c.unsubscribe_bars(&bar_type), "unsubscribe_bars"))
    }
    /// Declares a kernel feature (identical declarations share one) and subscribes to it at
    /// `cadence`; returns its id, which `on_feature` receives.
    #[pyo3(signature = (spec, cadence = None))]
    fn feature(
        &self,
        spec: &Bound<'_, PyFeatureSpec>,
        cadence: Option<&Bound<'_, PyCadence>>,
    ) -> PyResult<u32> {
        let spec = spec.get();
        let kind = spec.feature_kind()?;
        let id = spec.instrument_id;
        let cadence = cadence_of(cadence);
        self.with(|c| check(c.feature(kind, &id, cadence), "feature")).map(|f| f.0)
    }
    /// Records a value (str, int or Decimal; floats are refused) as a `StrategyRecord` output,
    /// which a replay compares.
    fn record(&self, tag: &str, value: &Bound<'_, PyAny>) -> PyResult<()> {
        let raw = raw9(value, "value")?;
        self.with(|c| check(c.record(tag, raw), "record"))
    }
    /// A copy of the instrument's book, or None without a book subscription.
    fn book(&self, instrument_id: &Bound<'_, PyAny>) -> PyResult<Option<PyBookView>> {
        let id = instrument_of(instrument_id)?;
        self.with(|c| Ok(c.book(&id).map(|v| PyBookView::snapshot(id, v))))
    }
    /// The instrument's definition, or None before one arrived.
    fn instrument<'py>(
        &self,
        py: Python<'py>,
        instrument_id: &Bound<'_, PyAny>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let id = instrument_of(instrument_id)?;
        match self.with(|c| Ok(c.instrument(&id).copied()))? {
            Some(i) => event_to_py(py, &Event::Instrument(i)),
            None => Ok(py.None().into_bound(py)),
        }
    }

    // ---- orders ---------------------------------------------------------------------------------

    /// A LIMIT order intent.
    #[staticmethod]
    #[pyo3(signature = (instrument_id, side, quantity, price, time_in_force = None, post_only = false, reduce_only = false))]
    fn limit(
        instrument_id: &Bound<'_, PyAny>,
        side: &Bound<'_, PyAny>,
        quantity: &Bound<'_, PyAny>,
        price: &Bound<'_, PyAny>,
        time_in_force: Option<&Bound<'_, PyAny>>,
        post_only: bool,
        reduce_only: bool,
    ) -> PyResult<PyOrderIntent> {
        let tif =
            optional_of::<TimeInForce>(time_in_force, "time_in_force")?.unwrap_or(TimeInForce::Gtc);
        let mut intent = OrderIntent::limit(
            instrument_of(instrument_id)?,
            OrderSide::from_py(side, "side")?,
            Quantity::from_py(quantity, "quantity")?,
            Price::from_py(price, "price")?,
            tif,
        );
        intent.post_only = post_only;
        intent.reduce_only = reduce_only;
        Ok(PyOrderIntent(intent))
    }
    /// A MARKET order intent (IOC).
    #[staticmethod]
    #[pyo3(signature = (instrument_id, side, quantity, reduce_only = false))]
    fn market(
        instrument_id: &Bound<'_, PyAny>,
        side: &Bound<'_, PyAny>,
        quantity: &Bound<'_, PyAny>,
        reduce_only: bool,
    ) -> PyResult<PyOrderIntent> {
        let mut intent = OrderIntent::market(
            instrument_of(instrument_id)?,
            OrderSide::from_py(side, "side")?,
            Quantity::from_py(quantity, "quantity")?,
        );
        intent.reduce_only = reduce_only;
        Ok(PyOrderIntent(intent))
    }
    /// Submits an order and returns its `ClientOrderId`. A denied order is not an error:
    /// `OrderDenied` arrives in `on_order_event` after this callback returns.
    fn submit(&self, intent: &Bound<'_, PyOrderIntent>) -> PyResult<PyClientOrderId> {
        let intent = intent.borrow().0;
        self.with(|c| check(c.submit(&intent), "submit")).map(PyClientOrderId)
    }
    /// Parent orders and execution algorithms are not in this tree yet (docs/rust-plan.md R3,
    /// second step); every algorithm is unknown here.
    #[pyo3(signature = (intent, algo = "passthrough", params = None))]
    fn submit_parent(
        &self,
        intent: &Bound<'_, PyOrderIntent>,
        algo: &str,
        params: Option<Vec<i64>>,
    ) -> PyResult<PyClientOrderId> {
        let _ = (intent, params);
        self.with(|_| Ok(()))?;
        Err(PyValueError::new_err(format!(
            "submit_parent: unknown execution algorithm \"{algo}\": parent orders and execution \
             algorithms are not in this build of jarvis (docs/rust-plan.md R3, second step)"
        )))
    }
    /// None: this tree has no parent orders.
    fn parent<'py>(
        &self,
        py: Python<'py>,
        parent_id: &Bound<'_, PyAny>,
    ) -> PyResult<Bound<'py, PyAny>> {
        ClientOrderId::from_py(parent_id, "parent_id")?;
        self.with(|_| Ok(()))?;
        Ok(py.None().into_bound(py))
    }
    /// Requests a new quantity and/or price for a LIMIT order. False when the order cannot be
    /// modified now (closed, or a cancel is pending).
    #[pyo3(signature = (client_order_id, quantity = None, price = None))]
    fn modify(
        &self,
        client_order_id: &Bound<'_, PyAny>,
        quantity: Option<&Bound<'_, PyAny>>,
        price: Option<&Bound<'_, PyAny>>,
    ) -> PyResult<bool> {
        let id = ClientOrderId::from_py(client_order_id, "client_order_id")?;
        let quantity = optional_of::<Quantity>(quantity, "quantity")?;
        let price = optional_of::<Price>(price, "price")?;
        self.with(|c| accepted(c.modify(&id, quantity, price), "modify"))
    }
    /// Requests a cancel. False when the order is closed or a cancel is already pending.
    fn cancel(&self, client_order_id: &Bound<'_, PyAny>) -> PyResult<bool> {
        let id = ClientOrderId::from_py(client_order_id, "client_order_id")?;
        self.with(|c| accepted(c.cancel(&id), "cancel"))
    }
    /// Cancels every cancelable order of this strategy (of one instrument when given); how many
    /// cancels were sent.
    #[pyo3(signature = (instrument_id = None))]
    fn cancel_all(&self, instrument_id: Option<&Bound<'_, PyAny>>) -> PyResult<u32> {
        let id = optional_of::<InstrumentId>(instrument_id, "instrument_id")?;
        self.with(|c| check(c.cancel_all(id.as_ref()), "cancel_all"))
    }
    /// A copy of one of this strategy's orders, or None.
    fn order(&self, client_order_id: &Bound<'_, PyAny>) -> PyResult<Option<PyOrderView>> {
        let id = ClientOrderId::from_py(client_order_id, "client_order_id")?;
        self.with(|c| Ok(c.order(&id).map(PyOrderView)))
    }
    /// Copies of this strategy's open orders (of one instrument when given), oldest first.
    #[pyo3(signature = (instrument_id = None))]
    fn open_orders(&self, instrument_id: Option<&Bound<'_, PyAny>>) -> PyResult<Vec<PyOrderView>> {
        let id = optional_of::<InstrumentId>(instrument_id, "instrument_id")?;
        self.with(|c| {
            let mut views = vec![OrderView::default(); 64];
            loop {
                let n = c.open_orders(id.as_ref(), &mut views);
                if n <= views.len() {
                    views.truncate(n);
                    break;
                }
                views.resize(n, OrderView::default());
            }
            Ok(views.into_iter().map(PyOrderView).collect())
        })
    }

    // ---- portfolio ------------------------------------------------------------------------------

    /// This strategy's position in the instrument, or None before the instrument is defined.
    fn position(&self, instrument_id: &Bound<'_, PyAny>) -> PyResult<Option<PyPositionView>> {
        let id = instrument_of(instrument_id)?;
        self.with(|c| {
            let size_precision = c.instrument(&id).map_or(9, |i| i.size_precision);
            Ok(c.position(&id).map(|view| PyPositionView { view, size_precision }))
        })
    }
    /// `open_exposure()` of the instrument: the account's position plus open orders.
    fn exposure(&self, instrument_id: &Bound<'_, PyAny>) -> PyResult<Option<PyExposureView>> {
        let id = instrument_of(instrument_id)?;
        self.with(|c| {
            let size_precision = c.instrument(&id).map_or(9, |i| i.size_precision);
            Ok(c.exposure(&id).map(|view| PyExposureView { view, size_precision }))
        })
    }
    /// The account's balance of one currency, or None.
    fn balance<'py>(
        &self,
        py: Python<'py>,
        currency: &Bound<'_, PyAny>,
    ) -> PyResult<Bound<'py, PyAny>> {
        let currency = Currency::from_py(currency, "currency")?;
        match self.with(|c| Ok(c.balance(currency)))? {
            Some(b) => b.to_py(py),
            None => Ok(py.None().into_bound(py)),
        }
    }
    /// Which commands the risk gates accept now: ACTIVE, REDUCING or HALTED.
    fn trading_state<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        self.with(|c| Ok(c.trading_state()))?.to_py(py)
    }
}

// ---- book view ----------------------------------------------------------------------------------

/// A copy of one instrument's book: the best levels of each side at the time of the callback.
#[pyclass(frozen, name = "BookView", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyBookView {
    instrument_id: InstrumentId,
    bids: Vec<BookLevel>,
    asks: Vec<BookLevel>,
    sequence: u64,
    ts_last: u64,
}

impl PyBookView {
    fn snapshot(instrument_id: InstrumentId, view: BookView<'_>) -> Self {
        let mut buf = [BookLevel::default(); BOOK_DEPTH];
        let n = view.bids(&mut buf);
        let bids = buf[..n.min(BOOK_DEPTH)].to_vec();
        let n = view.asks(&mut buf);
        let asks = buf[..n.min(BOOK_DEPTH)].to_vec();
        Self {
            instrument_id,
            bids,
            asks,
            sequence: view.sequence(),
            ts_last: view.ts_last().value(),
        }
    }
}

fn level(l: &BookLevel) -> (PyPrice, PyQuantity) {
    (PyPrice(l.price), PyQuantity(l.size))
}

#[pymethods]
impl PyBookView {
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.instrument_id)
    }
    /// `(price, size)` of the best bid, or None.
    fn best_bid(&self) -> Option<(PyPrice, PyQuantity)> {
        self.bids.first().map(level)
    }
    fn best_ask(&self) -> Option<(PyPrice, PyQuantity)> {
        self.asks.first().map(level)
    }
    /// Up to `depth` `(price, size)` levels, best first.
    #[pyo3(signature = (depth = 10))]
    fn bids(&self, depth: usize) -> Vec<(PyPrice, PyQuantity)> {
        self.bids.iter().take(depth).map(level).collect()
    }
    #[pyo3(signature = (depth = 10))]
    fn asks(&self, depth: usize) -> Vec<(PyPrice, PyQuantity)> {
        self.asks.iter().take(depth).map(level).collect()
    }
    /// Updates applied to the book so far.
    #[getter]
    fn sequence(&self) -> u64 {
        self.sequence
    }
    #[getter]
    fn ts_last(&self) -> u64 {
        self.ts_last
    }
    fn __repr__(&self) -> String {
        format!(
            "BookView({} bids={} asks={} sequence={})",
            self.instrument_id,
            self.bids.len(),
            self.asks.len(),
            self.sequence
        )
    }
}

// ---- batches ------------------------------------------------------------------------------------

static NUMPY: PyOnceLock<Py<PyModule>> = PyOnceLock::new();

fn numpy(py: Python<'_>) -> PyResult<&Bound<'_, PyModule>> {
    NUMPY.get_or_try_init(py, || py.import("numpy").map(Bound::unbind)).map(|m| m.bind(py))
}

/// A read-only one-dimensional numpy array of `dtype` over `bytes`.
fn column(
    py: Python<'_>,
    len: usize,
    dtype: &str,
    fill: impl FnOnce(&mut [u8]),
) -> PyResult<Py<PyAny>> {
    let bytes = PyBytes::new_with(py, len, |b| {
        fill(b);
        Ok(())
    })?;
    Ok(numpy(py)?.getattr("frombuffer")?.call1((bytes, dtype))?.unbind())
}

fn column_i64(py: Python<'_>, values: &[i64]) -> PyResult<Py<PyAny>> {
    column(py, values.len() * 8, "int64", |b| {
        for (chunk, v) in b.chunks_exact_mut(8).zip(values) {
            chunk.copy_from_slice(&v.to_ne_bytes());
        }
    })
}

fn column_u64(py: Python<'_>, values: &[u64]) -> PyResult<Py<PyAny>> {
    column(py, values.len() * 8, "uint64", |b| {
        for (chunk, v) in b.chunks_exact_mut(8).zip(values) {
            chunk.copy_from_slice(&v.to_ne_bytes());
        }
    })
}

fn column_u8(py: Python<'_>, values: &[u8]) -> PyResult<Py<PyAny>> {
    column(py, values.len(), "uint8", |b| b.copy_from_slice(values))
}

/// The trades of one instrument in one batch, as read-only columns: `ts_init` (uint64),
/// `price_raw` (int64, 1e9 scale), `size_raw` (uint64), `aggressor_side` (uint8).
#[pyclass(name = "TradeBatch", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyTradeBatch {
    instrument_id: InstrumentId,
    columns: [Py<PyAny>; 4],
    price_precision: u8,
    size_precision: u8,
    len: usize,
}

#[pymethods]
impl PyTradeBatch {
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.instrument_id)
    }
    #[getter]
    fn ts_init(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[0].clone_ref(py)
    }
    #[getter]
    fn price_raw(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[1].clone_ref(py)
    }
    #[getter]
    fn size_raw(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[2].clone_ref(py)
    }
    #[getter]
    fn aggressor_side(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[3].clone_ref(py)
    }
    #[getter]
    fn price_precision(&self) -> u8 {
        self.price_precision
    }
    #[getter]
    fn size_precision(&self) -> u8 {
        self.size_precision
    }
    fn __len__(&self) -> usize {
        self.len
    }
}

/// The quotes of one instrument in one batch, as read-only columns: `ts_init`, `bid_raw`,
/// `ask_raw` (int64), `bid_size_raw`, `ask_size_raw` (uint64).
#[pyclass(name = "QuoteBatch", module = "jarvis._core.node")]
#[derive(Debug)]
pub(crate) struct PyQuoteBatch {
    instrument_id: InstrumentId,
    columns: [Py<PyAny>; 5],
    price_precision: u8,
    size_precision: u8,
    len: usize,
}

#[pymethods]
impl PyQuoteBatch {
    #[getter]
    fn instrument_id(&self) -> PyInstrumentId {
        PyInstrumentId(self.instrument_id)
    }
    #[getter]
    fn ts_init(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[0].clone_ref(py)
    }
    #[getter]
    fn bid_raw(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[1].clone_ref(py)
    }
    #[getter]
    fn ask_raw(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[2].clone_ref(py)
    }
    #[getter]
    fn bid_size_raw(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[3].clone_ref(py)
    }
    #[getter]
    fn ask_size_raw(&self, py: Python<'_>) -> Py<PyAny> {
        self.columns[4].clone_ref(py)
    }
    #[getter]
    fn price_precision(&self) -> u8 {
        self.price_precision
    }
    #[getter]
    fn size_precision(&self) -> u8 {
        self.size_precision
    }
    fn __len__(&self) -> usize {
        self.len
    }
}

// ---- the host -----------------------------------------------------------------------------------

/// The object's reference count (the safe accessor; its replacement is an `unsafe` ffi call).
#[allow(deprecated)]
fn refcount(obj: &Bound<'_, PyAny>) -> isize {
    obj.get_refcnt()
}

/// One Python strategy object as a kernel [`Strategy`].
pub(crate) struct PyStrategyHost {
    id: String,
    callbacks: Vec<Option<Py<PyAny>>>,
    context: Py<PyContext>,
    save: Option<Py<PyAny>>,
    load: Option<Py<PyAny>>,
    options: HostOptions,
    stats: SharedStats,
    consecutive_overruns: u64,
}

impl core::fmt::Debug for PyStrategyHost {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("PyStrategyHost").field("id", &self.id).finish_non_exhaustive()
    }
}

/// `getattr(obj, name)` when present and not None.
fn optional_attr(obj: &Bound<'_, PyAny>, name: &str) -> PyResult<Option<Py<PyAny>>> {
    match obj.getattr(name) {
        Ok(v) if v.is_none() => Ok(None),
        Ok(v) => Ok(Some(v.unbind())),
        Err(e) if e.is_instance_of::<PyAttributeError>(obj.py()) => Ok(None),
        Err(e) => Err(e),
    }
}

/// The traceback of `err`, at most `MAX_TRACEBACK` characters.
fn traceback_text(py: Python<'_>, err: &PyErr) -> String {
    let formatted = py
        .import("traceback")
        .and_then(|m| m.getattr("format_exception"))
        .and_then(|f| f.call1((err.get_type(py), err.value(py), err.traceback(py))))
        .and_then(|lines| lines.extract::<Vec<String>>())
        .map_or_else(|_| err.to_string(), |lines| lines.concat());
    match formatted.char_indices().nth(MAX_TRACEBACK) {
        Some((cut, _)) => formatted[..cut].to_string(),
        None => formatted,
    }
}

impl PyStrategyHost {
    /// Adapts `strategy`; its `id` attribute (else its class name) names it in messages.
    pub(crate) fn new(strategy: &Bound<'_, PyAny>, options: HostOptions) -> PyResult<Self> {
        let py = strategy.py();
        let id = match optional_attr(strategy, "id")? {
            Some(id) => id.bind(py).str()?.to_string(),
            None => strategy.get_type().qualname()?.to_string(),
        };
        let callbacks = CALLBACKS
            .iter()
            .map(|name| optional_attr(strategy, name))
            .collect::<PyResult<Vec<_>>>()?;
        Ok(Self {
            id,
            callbacks,
            context: Py::new(py, PyContext::new())?,
            save: optional_attr(strategy, "on_save")?,
            load: optional_attr(strategy, "on_load")?,
            options,
            stats: Rc::new(Cell::new(HostStats::default())),
            consecutive_overruns: 0,
        })
    }

    pub(crate) fn id(&self) -> &str {
        &self.id
    }

    /// The counters, readable after the engine owns the host.
    pub(crate) fn stats(&self) -> SharedStats {
        Rc::clone(&self.stats)
    }

    fn has(&self, cb: usize) -> bool {
        self.callbacks[cb].is_some()
    }

    /// Calls callback `cb` with the context and `build`'s arguments; a failure while building
    /// the arguments is a failure of the strategy, like one raised in the callback.
    fn deliver(
        &mut self,
        ctx: &mut Context<'_>,
        cb: usize,
        build: impl for<'py> FnOnce(Python<'py>) -> PyResult<Vec<Bound<'py, PyAny>>>,
    ) -> KResult<()> {
        if !self.has(cb) {
            return Ok(());
        }
        Python::attach(|py| match build(py) {
            Ok(args) => self.invoke(py, ctx, cb, &args),
            Err(e) => self.failed(py, ctx, cb, &e),
        })
    }

    /// The call itself, with the context bound for its duration.
    fn invoke(
        &mut self,
        py: Python<'_>,
        ctx: &mut Context<'_>,
        cb: usize,
        args: &[Bound<'_, PyAny>],
    ) -> KResult<()> {
        let Some(f) = &self.callbacks[cb] else { return Ok(()) };
        let watch = Stopwatch::start();
        let mut all = Vec::with_capacity(args.len() + 1);
        all.push(self.context.bind(py).clone().into_any());
        all.extend(args.iter().cloned());
        let result = match PyTuple::new(py, all) {
            Ok(tuple) => {
                let slot = NonNull::from(&mut *ctx).cast::<Context<'static>>();
                let context = self.context.bind(py);
                context.borrow().slot.set(Some(slot));
                let r = f.bind(py).call1(tuple);
                context.borrow().slot.set(None);
                r.map(drop)
            }
            Err(e) => Err(e),
        };
        let status = match result {
            Ok(()) => Ok(()),
            Err(e) => self.failed(py, ctx, cb, &e),
        };
        self.measure(ctx, watch.elapsed_ns());
        status
    }

    /// Prints the failure, names it to the kernel and fails the callback.
    fn failed(
        &mut self,
        py: Python<'_>,
        ctx: &mut Context<'_>,
        cb: usize,
        err: &PyErr,
    ) -> KResult<()> {
        let type_name =
            err.get_type(py).qualname().map_or_else(|_| "Exception".to_string(), |q| q.to_string());
        eprintln!(
            "jarvis: strategy {} raised {type_name} in {} at seq {}:\n{}",
            self.id,
            CALLBACKS[cb],
            ctx.seq(),
            traceback_text(py, err)
        );
        ctx.report_error(
            StrategyErrorKind::Exception,
            failure_hash(&format!("python:{type_name}")),
        );
        Err(Status::InvalidState)
    }

    fn measure(&mut self, ctx: &mut Context<'_>, ns: u64) {
        let mut stats = self.stats.get();
        stats.calls += 1;
        stats.total_ns = stats.total_ns.saturating_add(ns);
        stats.max_ns = stats.max_ns.max(ns);
        if self.options.measure_overruns {
            if ns <= self.options.budget_ns {
                self.consecutive_overruns = 0;
            } else {
                stats.overruns += 1;
                self.consecutive_overruns += 1;
                if self.consecutive_overruns >= self.options.overrun_limit {
                    self.consecutive_overruns = 0;
                    ctx.report_error(StrategyErrorKind::Overrun, failure_hash("overrun"));
                }
            }
        }
        self.stats.set(stats);
    }

    /// A batch or one of its arrays still referenced after the callback would see the next
    /// batch's data (in this tree the arrays are copies, but the contract is the C++ one).
    fn report_escape(&mut self, ctx: &mut Context<'_>) {
        let mut stats = self.stats.get();
        stats.escapes += 1;
        self.stats.set(stats);
        eprintln!(
            "jarvis: strategy {} kept a batch or one of its arrays after the batch callback at \
             seq {}; the arrays are views that the next batch overwrites. Copy them \
             (numpy.array(x)) to keep the data.",
            self.id,
            ctx.seq()
        );
        ctx.report_error(StrategyErrorKind::Exception, failure_hash("jarvis.BatchEscaped"));
    }

    fn simple<T: PyField>(&mut self, ctx: &mut Context<'_>, cb: usize, value: &T) -> KResult<()> {
        self.deliver(ctx, cb, |py| Ok(vec![value.to_py(py)?]))
    }

    fn trade_batch(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        trades: &[TradeTick],
    ) -> KResult<()> {
        let Some(first) = trades.first() else { return Ok(()) };
        if !self.has(ON_TRADE_BATCH) {
            return Ok(());
        }
        Python::attach(|py| {
            let built = (|| -> PyResult<Bound<'_, PyTradeBatch>> {
                let ts: Vec<u64> = trades.iter().map(|t| t.ts_init.value()).collect();
                let price: Vec<i64> = trades.iter().map(|t| t.price.raw()).collect();
                let size: Vec<u64> = trades.iter().map(|t| t.size.raw()).collect();
                let side: Vec<u8> = trades.iter().map(|t| t.aggressor_side as u8).collect();
                Bound::new(
                    py,
                    PyTradeBatch {
                        instrument_id: *id,
                        columns: [
                            column_u64(py, &ts)?,
                            column_i64(py, &price)?,
                            column_u64(py, &size)?,
                            column_u8(py, &side)?,
                        ],
                        price_precision: first.price.precision(),
                        size_precision: first.size.precision(),
                        len: trades.len(),
                    },
                )
            })();
            let batch = match built {
                Ok(b) => b,
                Err(e) => return self.failed(py, ctx, ON_TRADE_BATCH, &e),
            };
            let status = self.invoke(py, ctx, ON_TRADE_BATCH, &[batch.clone().into_any()]);
            // The borrow below holds a reference of its own; the batch's count is read outside it.
            let kept_batch = refcount(batch.as_any()) > 1;
            let kept_column = batch.borrow().columns.iter().any(|c| refcount(c.bind(py)) > 1);
            if kept_batch || kept_column {
                self.report_escape(ctx);
            }
            for c in &mut batch.borrow_mut().columns {
                *c = py.None().into_any();
            }
            status
        })
    }

    fn quote_batch(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        quotes: &[QuoteTick],
    ) -> KResult<()> {
        let Some(first) = quotes.first() else { return Ok(()) };
        if !self.has(ON_QUOTE_BATCH) {
            return Ok(());
        }
        Python::attach(|py| {
            let built = (|| -> PyResult<Bound<'_, PyQuoteBatch>> {
                let ts: Vec<u64> = quotes.iter().map(|q| q.ts_init.value()).collect();
                let bid: Vec<i64> = quotes.iter().map(|q| q.bid_price.raw()).collect();
                let ask: Vec<i64> = quotes.iter().map(|q| q.ask_price.raw()).collect();
                let bid_size: Vec<u64> = quotes.iter().map(|q| q.bid_size.raw()).collect();
                let ask_size: Vec<u64> = quotes.iter().map(|q| q.ask_size.raw()).collect();
                Bound::new(
                    py,
                    PyQuoteBatch {
                        instrument_id: *id,
                        columns: [
                            column_u64(py, &ts)?,
                            column_i64(py, &bid)?,
                            column_i64(py, &ask)?,
                            column_u64(py, &bid_size)?,
                            column_u64(py, &ask_size)?,
                        ],
                        price_precision: first.bid_price.precision(),
                        size_precision: first.bid_size.precision(),
                        len: quotes.len(),
                    },
                )
            })();
            let batch = match built {
                Ok(b) => b,
                Err(e) => return self.failed(py, ctx, ON_QUOTE_BATCH, &e),
            };
            let status = self.invoke(py, ctx, ON_QUOTE_BATCH, &[batch.clone().into_any()]);
            // The borrow below holds a reference of its own; the batch's count is read outside it.
            let kept_batch = refcount(batch.as_any()) > 1;
            let kept_column = batch.borrow().columns.iter().any(|c| refcount(c.bind(py)) > 1);
            if kept_batch || kept_column {
                self.report_escape(ctx);
            }
            for c in &mut batch.borrow_mut().columns {
                *c = py.None().into_any();
            }
            status
        })
    }

    /// What `on_save` returned, as a length-prefixed byte string.
    fn write_saved(&self, out: &Bound<'_, PyAny>, w: &mut StateWriter<'_>) {
        let Ok(bytes) = out.cast::<PyBytes>() else {
            let type_name =
                out.get_type().qualname().map_or_else(|_| "object".to_string(), |q| q.to_string());
            eprintln!("jarvis: strategy {}: on_save must return bytes, not {type_name}", self.id);
            w.fail(Status::InvalidArgument);
            return;
        };
        let bytes = bytes.as_bytes();
        if let Ok(n) = u32::try_from(bytes.len()) {
            w.u32(n);
            w.raw(bytes);
        } else {
            w.fail(Status::CapacityExceeded);
        }
    }

    fn print_state_failure(&self, py: Python<'_>, what: &str, err: &PyErr) {
        let type_name =
            err.get_type(py).qualname().map_or_else(|_| "Exception".to_string(), |q| q.to_string());
        eprintln!(
            "jarvis: strategy {} raised {type_name} in {what}:\n{}",
            self.id,
            traceback_text(py, err)
        );
    }
}

impl Strategy for PyStrategyHost {
    fn on_start(&mut self, ctx: &mut Context<'_>) -> KResult<()> {
        self.deliver(ctx, ON_START, |_| Ok(Vec::new()))
    }
    fn on_stop(&mut self, ctx: &mut Context<'_>) -> KResult<()> {
        self.deliver(ctx, ON_STOP, |_| Ok(Vec::new()))
    }
    fn on_trade(&mut self, ctx: &mut Context<'_>, trade: &TradeTick) -> KResult<()> {
        self.simple(ctx, ON_TRADE, trade)
    }
    fn on_quote(&mut self, ctx: &mut Context<'_>, quote: &QuoteTick) -> KResult<()> {
        self.simple(ctx, ON_QUOTE, quote)
    }
    fn on_book(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        book: &BookView<'_>,
    ) -> KResult<()> {
        if !self.has(ON_BOOK) {
            return Ok(());
        }
        let view = PyBookView::snapshot(*id, *book);
        self.deliver(ctx, ON_BOOK, move |py| Ok(vec![Bound::new(py, view)?.into_any()]))
    }
    fn on_book_deltas(&mut self, ctx: &mut Context<'_>, deltas: &OrderBookDeltas) -> KResult<()> {
        if !self.has(ON_BOOK_DELTAS) {
            return Ok(());
        }
        let event = Event::OrderBookDeltas(deltas.clone());
        self.deliver(ctx, ON_BOOK_DELTAS, move |py| Ok(vec![event_to_py(py, &event)?]))
    }
    fn on_bar(&mut self, ctx: &mut Context<'_>, bar: &Bar) -> KResult<()> {
        self.simple(ctx, ON_BAR, bar)
    }
    fn on_mark_price(&mut self, ctx: &mut Context<'_>, update: &MarkPriceUpdate) -> KResult<()> {
        self.simple(ctx, ON_MARK_PRICE, update)
    }
    fn on_index_price(&mut self, ctx: &mut Context<'_>, update: &IndexPriceUpdate) -> KResult<()> {
        self.simple(ctx, ON_INDEX_PRICE, update)
    }
    fn on_funding_rate(
        &mut self,
        ctx: &mut Context<'_>,
        update: &FundingRateUpdate,
    ) -> KResult<()> {
        self.simple(ctx, ON_FUNDING_RATE, update)
    }
    fn on_instrument_status(
        &mut self,
        ctx: &mut Context<'_>,
        status: &InstrumentStatus,
    ) -> KResult<()> {
        self.simple(ctx, ON_INSTRUMENT_STATUS, status)
    }
    fn on_instrument_close(
        &mut self,
        ctx: &mut Context<'_>,
        close: &InstrumentClose,
    ) -> KResult<()> {
        self.simple(ctx, ON_INSTRUMENT_CLOSE, close)
    }
    fn on_liquidation(&mut self, ctx: &mut Context<'_>, order: &LiquidationOrder) -> KResult<()> {
        self.simple(ctx, ON_LIQUIDATION, order)
    }
    fn on_feature(
        &mut self,
        ctx: &mut Context<'_>,
        id: FeatureId,
        value: FeatureValue,
        _ts_event: UnixNanos,
        ts_init: UnixNanos,
    ) -> KResult<()> {
        self.deliver(ctx, ON_FEATURE, |py| {
            Ok(vec![
                id.0.into_pyobject(py)?.into_any(),
                decimal9(py, value.0)?,
                ts_init.value().into_pyobject(py)?.into_any(),
            ])
        })
    }
    fn on_trade_batch(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        trades: &[TradeTick],
    ) -> KResult<()> {
        self.trade_batch(ctx, id, trades)
    }
    fn on_quote_batch(
        &mut self,
        ctx: &mut Context<'_>,
        id: &InstrumentId,
        quotes: &[QuoteTick],
    ) -> KResult<()> {
        self.quote_batch(ctx, id, quotes)
    }
    fn on_order_event(&mut self, ctx: &mut Context<'_>, event: &OrderEvent) -> KResult<()> {
        self.deliver(ctx, ON_ORDER_EVENT, |py| Ok(vec![order_event_to_py(py, event)?]))
    }
    fn on_position_event(&mut self, ctx: &mut Context<'_>, event: &PositionEvent) -> KResult<()> {
        self.deliver(ctx, ON_POSITION_EVENT, |py| Ok(vec![position_event_to_py(py, event)?]))
    }
    fn on_error(&mut self, ctx: &mut Context<'_>, error: &StrategyError) -> KResult<()> {
        self.simple(ctx, ON_ERROR, error)
    }
    fn on_timer(
        &mut self,
        ctx: &mut Context<'_>,
        key: TimerKey,
        deadline: UnixNanos,
    ) -> KResult<()> {
        self.deliver(ctx, ON_TIMER, |py| {
            Ok(vec![
                key.id.into_pyobject(py)?.into_any(),
                deadline.value().into_pyobject(py)?.into_any(),
            ])
        })
    }

    /// Both `on_save` and `on_load` defined (docs/architecture.md section 16.3).
    fn has_state(&self) -> bool {
        self.save.is_some() && self.load.is_some()
    }
    /// `on_save(self) -> bytes`, outside any callback and without a context.
    fn save_state(&self, w: &mut StateWriter<'_>) {
        let Some(save) = &self.save else { return };
        Python::attach(|py| match save.bind(py).call0() {
            Ok(out) => self.write_saved(&out, w),
            Err(e) => {
                self.print_state_failure(py, "on_save", &e);
                w.fail(Status::InvalidState);
            }
        });
    }
    /// `on_load(self, state)` with the bytes `on_save` returned.
    fn load_state(&mut self, r: &mut StateReader<'_>) {
        let Some(load) = &self.load else { return };
        let n = r.u32() as usize;
        let bytes = r.raw(n);
        if !r.is_ok() {
            return;
        }
        Python::attach(|py| {
            if let Err(e) = load.bind(py).call1((PyBytes::new(py, bytes),)) {
                self.print_state_failure(py, "on_load", &e);
                r.fail(Status::InvalidState);
            }
        });
    }
}

pub(crate) fn add_classes(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyContext>()?;
    m.add_class::<PyBookView>()?;
    m.add_class::<PyTradeBatch>()?;
    m.add_class::<PyQuoteBatch>()?;
    Ok(())
}
