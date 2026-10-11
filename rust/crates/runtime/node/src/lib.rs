//! Runtime layer `node` (docs/architecture.md section 4): the composition root. Configuration,
//! the run directory, the segmented run log, snapshot files, catalog sources, the backtest node,
//! replay and the report. Sandbox and live wiring arrive with the live layer (docs/rust-plan.md
//! R5); the Python host with R4's second step.
#![forbid(unsafe_code)]

pub mod backtest_node;
pub mod build_info;
pub mod catalog;
pub mod config;
pub mod event_log;
pub mod log_source;
pub mod probe;
pub mod registry;
pub mod replay;
pub mod run_dir;
pub mod run_report;
pub mod snapshot_file;

use std::fmt;

use kernel_core::Status;

pub use backtest_node::{run_backtest, BacktestRequest, BacktestResult};
pub use config::{
    canonical_hashed_text, canonical_operational_text, config_hash, config_hash_hex, config_report,
    format_errors, load_config, parse_config, ConfigError, ConfigOverrides, NodeConfig,
};
pub use event_log::{EventLogOptions, EventLogReader, EventLogWriter};
pub use registry::Registry;
pub use replay::{replay_run, Divergence, ReplayOptions, ReplayReport};
pub use run_dir::{create_run_directory, load_run_config, run_header, RunManifest};
pub use run_report::{build_run_report, report_text, RunReport};

/// A failure of the node with what went wrong, for the person running it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NodeError {
    pub status: Status,
    pub message: String,
}

impl NodeError {
    #[must_use]
    pub fn new(status: Status, message: impl Into<String>) -> Self {
        Self { status, message: message.into() }
    }
}

impl fmt::Display for NodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for NodeError {}

impl From<Status> for NodeError {
    fn from(status: Status) -> Self {
        Self { status, message: status.to_string() }
    }
}

pub type NodeResult<T> = std::result::Result<T, NodeError>;
