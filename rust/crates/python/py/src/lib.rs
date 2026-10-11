//! `jarvis._core`: the Python extension of the Rust tree (docs/rust-plan.md R4). The package
//! `python/jarvis` is the same for the C++ and the Rust extension; this crate provides the
//! `_core` module it loads: `model` (value types, enums, events), `log` (event logs) and `node`
//! (strategies and the node), plus `build_info()`.
//!
//! `unsafe` appears in one place: `host::PyContext::with`, where a Python `Context` object holds a
//! pointer to the kernel context of the running callback (see the module's comment).

// The bindings enumerate the model: its identifier and event lists are long and the glob imports
// of those modules are deliberate; `__reduce__` returns the tuple pickle expects.
#![allow(clippy::wildcard_imports, clippy::type_complexity)]

mod common;
mod events;
mod host;
mod log;
mod model;
mod node;
mod values;
mod views;

use pyo3::prelude::*;
use pyo3::types::PyDict;

/// Identity of the native build: version, git commit, compiler, platform, live components.
#[pyfunction]
fn build_info(py: Python<'_>) -> PyResult<Bound<'_, PyDict>> {
    let d = PyDict::new(py);
    d.set_item("version", node::VERSION)?;
    d.set_item("git_commit", ::node::build_info::git_commit())?;
    d.set_item("compiler", format!("rustc (jarvis-rs {})", ::node::build_info::VERSION))?;
    d.set_item("platform", ::node::build_info::platform_text())?;
    d.set_item("live_enabled", false)?;
    Ok(d)
}

#[pymodule(name = "_core")]
fn core(py: Python<'_>, m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add("__doc__", "jarvis native extension: model types, event logs, strategies and the node")?;
    m.add_function(wrap_pyfunction!(build_info, m)?)?;
    let model = model::build(py)?;
    m.add_submodule(&model)?;
    let log = log::build(py)?;
    m.add_submodule(&log)?;
    let node = node::build(py)?;
    m.add_submodule(&node)?;
    let modules = py.import("sys")?.getattr("modules")?;
    modules.set_item("jarvis._core.log", &log)?;
    modules.set_item("jarvis._core.node", &node)?;
    Ok(())
}
