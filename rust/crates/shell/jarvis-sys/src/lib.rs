//! Operating system calls: durable files, sockets, shared libraries, stop signals, process, clocks
//!
//! Shell layer `sys` (docs/architecture.md section 3). Not yet ported; see docs/rust-plan.md.
//! Shell crates may use std, third-party crates and, where a layer needs it, `unsafe` in a module
//! that says why; errors are translated to `jarvis_core::Status` before entering the kernel.
#![forbid(unsafe_code)]
