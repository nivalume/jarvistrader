# ADR 0001: Deterministic kernel constraints

- Status: Accepted
- Date: 2026-08-11

## Context

The future kernel must produce identical state transitions and output traces for identical inputs,
independent of compiler, optimization level, and supported host platform. Determinism is a product
property, not a best-effort testing preference.

This ADR records the constraints now so that future domain code enters through a guarded build
rather than retrofitting determinism later. The current repository is a toolchain scaffold and has
no kernel state or fingerprint implementation.

## Decision

1. Kernel prices and quantities use integer tick and lot units represented by `int64_t`. Cash uses
   `__int128`. Floating-point state is forbidden; floating point is reserved for final metric
   presentation outside the kernel.
2. Market events use the strict total-order key `(ts, source_id, row)`. Resting orders use
   `(ts_visible, order_id)`, where `order_id` is unique and monotonically increasing.
3. State transition code must not read wall-clock time, global RNG state, pointer values, unordered
   iteration order, or environment variables.
4. Randomized latency and slippage use a counter-based generator keyed by `(seed, order_id)` and
   never depend on call order. Stateful standard-library generators are forbidden.
5. First-party targets compile with floating-point contraction disabled and without fast-math.
   Builds must not use `-march=native`.
6. Once state exists, the canonical trace format will use explicitly encoded fixed-width fields;
   it must never serialize padding, addresses, or container capacity. Release and O0 output will be
   compared byte-for-byte, with SHA-256 used only as a display summary.

## Enforcement status

The `jarvis::strict` interface target owns the common warnings and floating-point flags. The
`jarvis_kernel_freestanding` object target compiles with exceptions and RTTI disabled. CI builds
the Release and O0 paths independently. Byte comparison is deliberately deferred until the first
kernel state exists; the current CI does not report an empty trace as proof of determinism.

The project supports GCC, Clang, and AppleClang on Linux and macOS. MSVC is outside the supported
platform set because `__int128` is a deliberate kernel requirement.
