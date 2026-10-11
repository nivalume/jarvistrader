//! `jarvis._core.log`: write events to a log directory, read records back as model objects,
//! fingerprint and compare logs, and generate the deterministic corpus.

use std::path::Path;

use kernel_core::{EventKey, FixedString, UnixNanos};
use model::log::{Fingerprinter, LogHeader, Record, RecordBody};
use node::event_log::{EventLogOptions, EventLogReadOptions, EventLogReader, EventLogWriter};
use pyo3::exceptions::PyValueError;
use pyo3::prelude::*;
use pyo3::types::{PyBytes, PyDict, PyModule};

use crate::common::{check, status_error};
use crate::events::{
    event_class_name, event_from_py, event_to_py, output_class_name, output_from_py, output_to_py,
};

fn set_text<const N: usize>(text: &str) -> FixedString<N> {
    let mut end = text.len().min(N);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    FixedString::from_text(&text[..end]).unwrap_or_default()
}

/// The interpreter's `major.minor.micro`.
pub(crate) fn python_version(py: Python<'_>) -> PyResult<String> {
    let v = py.import("sys")?.getattr("version_info")?;
    Ok(format!(
        "{}.{}.{}",
        v.getattr("major")?.extract::<u32>()?,
        v.getattr("minor")?.extract::<u32>()?,
        v.getattr("micro")?.extract::<u32>()?
    ))
}

pub(crate) fn numpy_version(py: Python<'_>) -> String {
    py.import("numpy")
        .and_then(|m| m.getattr("__version__"))
        .and_then(|v| v.extract::<String>())
        .unwrap_or_default()
}

/// `PYTHONHASHSEED`, when set to a number.
pub(crate) fn python_hash_seed(py: Python<'_>) -> PyResult<Option<u32>> {
    let environ = py.import("os")?.getattr("environ")?;
    let value: String = environ.call_method1("get", ("PYTHONHASHSEED", ""))?.extract()?;
    Ok(value.parse::<u32>().ok())
}

/// A header carrying the build and the Python host's identity.
pub(crate) fn make_header(
    py: Python<'_>,
    config_hash: [u8; 32],
    seed: u64,
    node_tag: &str,
    env: &str,
    numpy: &str,
) -> PyResult<LogHeader> {
    let mut h = check(LogHeader::new(config_hash, seed, "", ""), "EventLogWriter")?;
    h.node_tag = set_text(node_tag);
    h.env = set_text(env);
    h.build = set_text(&node::build_info::build_text());
    h.platform = set_text(&node::build_info::platform_text());
    h.python_version = set_text(&python_version(py)?);
    let numpy_text = if numpy.is_empty() { numpy_version(py) } else { numpy.to_string() };
    h.numpy_version = set_text(&numpy_text);
    h.python_hash_seed = python_hash_seed(py)?;
    Ok(h)
}

fn header_dict<'py>(py: Python<'py>, h: &LogHeader) -> PyResult<Bound<'py, PyDict>> {
    let d = PyDict::new(py);
    d.set_item("format_version", h.format_version)?;
    d.set_item("schema_version", h.schema_version)?;
    d.set_item("segment_index", 0u32)?;
    d.set_item("config_hash", PyBytes::new(py, &h.config_hash))?;
    d.set_item("seed", h.seed)?;
    d.set_item("python_hash_seed", h.python_hash_seed)?;
    d.set_item("jarvis_version", h.build.as_str())?;
    d.set_item("git_commit", node::build_info::git_commit())?;
    d.set_item("compiler", "rustc")?;
    d.set_item("platform", h.platform.as_str())?;
    d.set_item("python_version", h.python_version.as_str())?;
    d.set_item("numpy_version", h.numpy_version.as_str())?;
    d.set_item("node_tag", h.node_tag.as_str())?;
    d.set_item("env", h.env.as_str())?;
    d.set_item("sbe_schema", "")?;
    Ok(d)
}

/// Writes events to a new event log directory (refuses to overwrite).
#[pyclass(name = "EventLogWriter", module = "jarvis._core.log")]
#[derive(Debug)]
pub struct PyLogWriter {
    writer: Option<EventLogWriter>,
    directory: String,
}

#[pymethods]
impl PyLogWriter {
    #[new]
    #[pyo3(signature = (directory, *, seed = 0, config_hash = None, segment_bytes = 256 << 20, sync_on_flush = false, numpy_version = ""))]
    fn new(
        py: Python<'_>,
        directory: &str,
        seed: u64,
        config_hash: Option<&[u8]>,
        segment_bytes: u64,
        sync_on_flush: bool,
        numpy_version: &str,
    ) -> PyResult<Self> {
        let mut hash = [0u8; 32];
        if let Some(bytes) = config_hash {
            if !bytes.is_empty() && bytes.len() != 32 {
                return Err(PyValueError::new_err(
                    "config_hash must be 32 bytes (a SHA-256 digest)",
                ));
            }
            if bytes.len() == 32 {
                hash.copy_from_slice(bytes);
            }
        }
        let header = make_header(py, hash, seed, "", "", numpy_version)?;
        let options = EventLogOptions { segment_bytes, sync_on_flush, durable: false };
        let writer = EventLogWriter::open(Path::new(directory), &header, options)
            .map_err(|s| status_error(&format!("EventLogWriter({directory})"), s))?;
        Ok(Self { writer: Some(writer), directory: directory.to_string() })
    }

    /// Appends an input event, or a kernel output, under the order key `(ts, source_id, seq)`.
    #[pyo3(signature = (event, *, seq, ts, source_id = 0))]
    fn append(
        &mut self,
        event: &Bound<'_, PyAny>,
        seq: u64,
        ts: u64,
        source_id: u16,
    ) -> PyResult<()> {
        let key = EventKey::new(UnixNanos::new(ts), source_id, seq);
        if let Some(output) = output_from_py(event)? {
            return check(self.writer()?.append_output(key, &output), "EventLogWriter.append");
        }
        let e = event_from_py(event)?;
        check(self.writer()?.append(key, &e), "EventLogWriter.append")
    }
    fn flush(&mut self) -> PyResult<()> {
        check(self.writer()?.flush(), "EventLogWriter.flush")
    }
    fn close(&mut self) -> PyResult<()> {
        if let Some(w) = self.writer.as_mut() {
            check(w.close(), "EventLogWriter.close")?;
        }
        Ok(())
    }
    #[getter]
    fn records(&self) -> u64 {
        self.writer.as_ref().map_or(0, EventLogWriter::records)
    }
    #[getter]
    fn directory(&self) -> &str {
        &self.directory
    }
    fn __enter__(slf: Bound<'_, Self>) -> Bound<'_, Self> {
        slf
    }
    #[pyo3(signature = (exc_type = None, exc = None, traceback = None))]
    fn __exit__(
        &mut self,
        exc_type: Option<&Bound<'_, PyAny>>,
        exc: Option<&Bound<'_, PyAny>>,
        traceback: Option<&Bound<'_, PyAny>>,
    ) -> PyResult<bool> {
        let _ = (exc_type, exc, traceback);
        self.close()?;
        Ok(false)
    }
}

impl PyLogWriter {
    fn writer(&mut self) -> PyResult<&mut EventLogWriter> {
        self.writer.as_mut().ok_or_else(|| PyValueError::new_err("EventLogWriter is closed"))
    }
}

/// One log record: its order key, kind and event.
#[pyclass(frozen, name = "Record", module = "jarvis._core.log")]
#[derive(Debug)]
pub struct PyRecord {
    #[pyo3(get)]
    seq: u64,
    #[pyo3(get)]
    ts: u64,
    #[pyo3(get)]
    source_id: u16,
    #[pyo3(get)]
    kind: String,
    #[pyo3(get)]
    event: Py<PyAny>,
    #[pyo3(get)]
    is_output: bool,
}

#[pymethods]
impl PyRecord {
    fn __repr__(&self) -> String {
        format!(
            "Record(seq={}, ts={}, source_id={}, kind='{}')",
            self.seq, self.ts, self.source_id, self.kind
        )
    }
}

pub(crate) fn record_to_py(py: Python<'_>, r: &Record) -> PyResult<PyRecord> {
    let (kind, event, is_output) = match &r.body {
        RecordBody::Input(e) => (event_class_name(e).to_string(), event_to_py(py, e)?, false),
        RecordBody::Output(o) => (output_class_name(o).to_string(), output_to_py(py, o)?, true),
    };
    Ok(PyRecord {
        seq: r.key.seq,
        ts: r.key.ts.value(),
        source_id: r.key.source_id,
        kind,
        event: event.unbind(),
        is_output,
    })
}

/// Iterates the records of an event log directory.
#[pyclass(name = "EventLogReader", module = "jarvis._core.log")]
#[derive(Debug)]
pub struct PyLogReader {
    reader: EventLogReader,
}

#[pymethods]
impl PyLogReader {
    #[new]
    fn new(directory: &str) -> PyResult<Self> {
        let reader = EventLogReader::open(Path::new(directory), EventLogReadOptions::default())
            .map_err(|s| status_error(&format!("EventLogReader({directory})"), s))?;
        Ok(Self { reader })
    }
    #[getter]
    fn header<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyDict>> {
        header_dict(py, self.reader.header())
    }
    fn __iter__(slf: PyRef<'_, Self>) -> PyRef<'_, Self> {
        slf
    }
    fn __next__(mut slf: PyRefMut<'_, Self>) -> PyResult<Option<PyRecord>> {
        let py = slf.py();
        match check(slf.reader.next_record(), "EventLogReader")? {
            None => Ok(None),
            Some(r) => Ok(Some(record_to_py(py, &r)?)),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Filter {
    Inputs,
    Outputs,
    All,
}

fn filter_of(records: &str) -> PyResult<Filter> {
    match records {
        "inputs" => Ok(Filter::Inputs),
        "outputs" => Ok(Filter::Outputs),
        "all" => Ok(Filter::All),
        _ => Err(PyValueError::new_err("records must be 'inputs', 'outputs' or 'all'")),
    }
}

fn selected(filter: Filter, r: &Record) -> bool {
    matches!(
        (filter, &r.body),
        (Filter::All, _)
            | (Filter::Inputs, RecordBody::Input(_))
            | (Filter::Outputs, RecordBody::Output(_))
    )
}

fn open(directory: &str, what: &str) -> PyResult<EventLogReader> {
    EventLogReader::open(Path::new(directory), EventLogReadOptions::default())
        .map_err(|s| status_error(&format!("{what}: {directory}"), s))
}

/// `(sha256 hex, record count)` over the selected records' bytes; the header is excluded.
#[pyfunction]
#[pyo3(signature = (directory, records = "inputs"))]
fn fingerprint(directory: &str, records: &str) -> PyResult<(String, u64)> {
    let filter = filter_of(records)?;
    let reader = open(directory, "fingerprint")?;
    let mut fp = Fingerprinter::new();
    for record in reader {
        let record = check(record, "fingerprint")?;
        if selected(filter, &record) {
            fp.add_record(&record);
        }
    }
    let f = fp.finish();
    let hex = kernel_core::sha256::hex(&f.digest);
    Ok((String::from_utf8_lossy(&hex).into_owned(), f.records))
}

fn record_bytes(r: &Record) -> Vec<u8> {
    let mut w = model::log::LogWriter::continuation();
    let _ = match &r.body {
        RecordBody::Input(e) => w.append(r.key, e),
        RecordBody::Output(o) => w.append_output(r.key, o),
    };
    w.into_bytes()
}

fn record_name(r: &Record) -> &'static str {
    match &r.body {
        RecordBody::Input(e) => event_class_name(e),
        RecordBody::Output(o) => output_class_name(o),
    }
}

/// `(equal, records compared, index of the first difference, description)`.
#[pyfunction]
#[pyo3(signature = (a, b, records = "inputs"))]
fn compare(a: &str, b: &str, records: &str) -> PyResult<(bool, u64, u64, String)> {
    let filter = filter_of(records)?;
    let mut ra = open(a, "compare")?.filter(|r| r.as_ref().map_or(true, |r| selected(filter, r)));
    let mut rb = open(b, "compare")?.filter(|r| r.as_ref().map_or(true, |r| selected(filter, r)));
    let mut compared = 0u64;
    loop {
        let x = ra.next().transpose().map_err(|s| status_error("compare", s))?;
        let y = rb.next().transpose().map_err(|s| status_error("compare", s))?;
        match (x, y) {
            (None, None) => return Ok((true, compared, 0, String::new())),
            (Some(x), None) => {
                return Ok((
                    false,
                    compared,
                    compared,
                    format!("{a} has more records: {} at seq {}", record_name(&x), x.key.seq),
                ));
            }
            (None, Some(y)) => {
                return Ok((
                    false,
                    compared,
                    compared,
                    format!("{b} has more records: {} at seq {}", record_name(&y), y.key.seq),
                ));
            }
            (Some(x), Some(y)) => {
                if record_bytes(&x) != record_bytes(&y) {
                    return Ok((
                        false,
                        compared,
                        compared,
                        format!(
                            "record {compared}: {} seq {} ts {} vs {} seq {} ts {}",
                            record_name(&x),
                            x.key.seq,
                            x.key.ts.value(),
                            record_name(&y),
                            y.key.seq,
                            y.key.ts.value()
                        ),
                    ));
                }
                compared += 1;
            }
        }
    }
}

/// Writes the deterministic corpus (every event kind) used by the determinism gate.
#[pyfunction]
fn write_corpus(py: Python<'_>, directory: &str, seed: u64, events: u64) -> PyResult<()> {
    let header = make_header(
        py,
        kernel_core::sha256::Sha256::digest(b"corpus"),
        seed,
        "corpus",
        "corpus",
        "",
    )?;
    let mut writer =
        EventLogWriter::open(Path::new(directory), &header, EventLogOptions::default())
            .map_err(|s| status_error("write_corpus", s))?;
    for record in corpus::Corpus::new(seed).records(events) {
        let (key, event) = check(record, "write_corpus")?;
        check(writer.append(key, &event), "write_corpus")?;
    }
    check(writer.close(), "write_corpus")
}

/// The event's line as `jarvis-rs dump` prints it.
#[pyfunction]
fn event_text(event: &Bound<'_, PyAny>) -> PyResult<String> {
    crate::events::event_text(event)
}

pub(crate) fn build(py: Python<'_>) -> PyResult<Bound<'_, PyModule>> {
    let m = PyModule::new(py, "log")?;
    m.add("__doc__", "Event logs (docs/architecture.md section 16)")?;
    m.add_class::<PyLogWriter>()?;
    m.add_class::<PyLogReader>()?;
    m.add_class::<PyRecord>()?;
    m.add_function(wrap_pyfunction!(fingerprint, &m)?)?;
    m.add_function(wrap_pyfunction!(compare, &m)?)?;
    m.add_function(wrap_pyfunction!(write_corpus, &m)?)?;
    m.add_function(wrap_pyfunction!(event_text, &m)?)?;
    Ok(m)
}
