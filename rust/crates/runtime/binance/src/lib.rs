//! Binance USDS-M adapter: codecs, streams, user stream, WS API, REST, instruments
//!
//! Runtime layer `adapter/binance` (the "shell" of docs/architecture.md section 3). Not yet ported; see
//! docs/rust-plan.md.
//! Runtime crates may use std, third-party crates and, where a layer needs it, `unsafe` in a module
//! that says why; errors are translated to `kernel_core::Status` before entering the kernel.
#![forbid(unsafe_code)]
