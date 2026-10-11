//! Runtime layer `sys` (docs/architecture.md section 3): the one layer that calls the operating
//! system. Everything above it receives time as a value and files as bytes.
//!
//! - [`file`]: files written to survive a crash (the event log, snapshots): synced through the
//!   operating system's handle and replaced atomically.
//! - [`time`]: the wall clock (only the run id reads it), the monotonic stopwatch, and UTC
//!   calendar text.
//!
//! Sockets, shared libraries and stop signals arrive with the live layer (docs/rust-plan.md R5).
//! Errors are translated to `kernel_core::Status` before entering the kernel.
#![forbid(unsafe_code)]

pub mod file;
pub mod time;

pub use file::{
    read_bytes, read_text, replace_file, sync_directory, write_text_atomic, DurableFile, Mode,
};
pub use time::{run_id, wall_now, Stopwatch};
