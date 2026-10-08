//! Build info, node configuration, node composition, command-line entry points
//!
//! Shell layer `node` (docs/architecture.md section 3). Not yet ported; see docs/rust-plan.md.
//! Shell crates may use std, third-party crates and, where a layer needs it, `unsafe` in a module
//! that says why; errors are translated to `kernel_core::Status` before entering the kernel.
#![forbid(unsafe_code)]
