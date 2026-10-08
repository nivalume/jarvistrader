//! Binance USDS-M adapter: codecs, streams, user stream, WS API, REST, instruments
//!
//! Shell layer `adapter/binance` (docs/architecture.md section 3). Not yet ported; see docs/rust-plan.md.
//! Shell crates may use std, third-party crates and, where a layer needs it, `unsafe` in a module
//! that says why; errors are translated to `jarvis_core::Status` before entering the kernel.
#![forbid(unsafe_code)]
