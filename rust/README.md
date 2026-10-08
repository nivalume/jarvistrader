# jarvis, the Rust tree

The Rust port of `jarvis` (the rationale is `docs/rust-migration.md`, the plan `docs/rust-plan.md`).
It is built next to the C++ tree, which stays the running system until the plan's R6.

```sh
cd rust
cargo test                              # dev profile
cargo test --release                    # overflow-checks stay on
cargo test --profile det-o0             # release semantics at opt-level 0: the determinism pair
cargo clippy --all-targets -- -D warnings
cargo check -p core --target x86_64-unknown-none   # the kernel needs no std
cargo run -p cli -- build-info
```

Layout (one crate per architecture layer, docs/architecture.md section 3):

- `crates/kernel/`: `core` (fixed-capacity containers, `SlotMap` with generational
  handles, counter-based RNG, time, timers, checksums, snapshot encoding) and `model` (done: value types, identifiers, enums,
  data and event types, the event log wire format, fingerprints), `data` (interning, the
  subscription matrix and cadences, routing, tick-indexed order books, bar aggregation, the
  feature graph), `cost` (fee schedules and funding, book-depth slippage, jittered latency) and
  `portfolio` (netting positions, the per-strategy ledger, balances, margin, funding), then
  `execution`, `risk`, `strategy`, `engine`, `backtest` (skeletons). Every kernel crate is
  `#![no_std]`, `#![forbid(unsafe_code)]`, `#![deny(clippy::float_arithmetic)]`, and the
  directory's `clippy.toml` bans `HashMap`, `HashSet` and `BTreeMap`. Dependencies between them
  follow the layer table; Cargo refuses a cycle, which is what `tools/check-layering.py` checks
  in the C++ tree.
- `crates/runtime/`: `sys`, `node`, `network`, `binance`, `live` (skeletons). std and third-party
  crates allowed; `unsafe` only in a module that says why.
- `crates/testkit/corpus`: the deterministic corpus of kernel inputs (every event kind, every
  order event variant, a pure function of the seed). Test support, not kernel code: it drives the
  encoding and log tests in its own `tests/`, seeds the fuzzers, and is what the determinism gate
  fingerprints. It sits above `model`, so `model`'s own tests cannot use it (a dev-dependency
  cycle would build `model` twice); tests that need it live here.
- `crates/testkit/testkit`: the property-test generator (same splitmix64 stream as the
  C++ `testkit::Gen`), `for_all` with `JARVIS_PROP_SEED` / `JARVIS_PROP_ITERS` /
  `JARVIS_PROP_CASE`, and the counting allocator behind the zero-allocation gate.
- `crates/tools/cli`: the `jarvis-rs` binary: `corpus`, `fingerprint`, `dump`,
  `roundtrip`, `sha256`, `crc32c`, `build-info`; more subcommands arrive with their layers.
- `tests/golden/`: the Rust tree's golden fingerprints (see its README).
- `tools/gen_conformance.py`: turns `tests/conformance/nautilus_cd417b80.json` into the
  currency table and the conformance test vectors; CI checks the output is current.

The C++ tree is a design reference, not an oracle: the Rust tree implements the contract in
`docs/architecture.md` on its own and is verified by its tests, the TLA+ specifications and the
determinism gate (`just rust-fp`: the `release` and `det-o0` builds write the same corpus byte for
byte).

`overflow-checks` is on in every profile: an overflow is a bug (ADR 0001), and it must fail the
same way in `release` and `det-o0`.
