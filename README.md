# jarvis

`jarvis` is the buildable project scaffold for a deterministic, event-driven C++20
backtesting kernel exposed to Python through nanobind.

This repository intentionally contains no trading-domain implementation yet. The native module,
freestanding target, test target, and benchmark target exist only to prove that the toolchain is
wired correctly.

## Requirements

- CMake 3.25 or newer
- Ninja
- GCC 13+, Clang 17+, or AppleClang 15+ (the `fuzz` and `tsan` presets need Clang's runtime
  libraries, `libclang-rt-18-dev` on Ubuntu)
- Python 3.11 or newer
- `uv` and `just` for the documented local workflow (`uv tool install rust-just` provides `just`)
- clang-format and clang-tidy for linting
- Java 17 or newer for the TLA+ model checker; `tools/tla/run_tlc.py` downloads the pinned
  `tla2tools.jar` on first use

MSVC is intentionally unsupported because the planned kernel requires `__int128`.

## Local workflow

```sh
just bootstrap
just check        # lint, functional tests, golden, fingerprints, benchmark A/B, affected TLA+ specs
```

Individual tiers: `just test`, `just golden`, `just zero-alloc`, `just layering`, `just bench`,
`just bench-compare base=main`, `just tla`, `just tla-changed`, `just fuzz target=smoke`,
`just tsan`. Recipes whose feature has not landed yet print `SKIPPED` and name the milestone in
`docs/plan.md` that adds it. The gate order and what each tier checks are described in
`docs/architecture.md` section 17.

The equivalent packaging check is:

```sh
uv venv --python 3.11 .venv
uv pip install --python .venv/bin/python --reinstall .
.venv/bin/python -c 'import jarvis; print(jarvis._core.build_info())'
```

## Layout

- `jarvis/`: header-only kernel layers and the `jarvis_shell` layers (`node`, `network`, `adapter`,
  `live`); empty modules are retained with `.gitkeep`
- `python/`: minimal nanobind and Python package boundary
- `testkit/`: property-test generator, zero-allocation counter, doctest entry point, freestanding guard
- `tests/`: C++ tests per layer, golden traces, fuzz targets, and pytest for the tools
- `benchmarks/`: gating (`hot/`) and report-only (`report/`) benchmarks with `thresholds.toml`
- `specs/`: TLA+ specs and `specs/tla/MAP.toml`
- `tools/`: layering checker, golden runner, benchmark A/B, TLA+ runner and spec selector
- `docs/`: deterministic-kernel constraints and C++ subset; the system design is in
  [`docs/architecture.md`](docs/architecture.md) and the milestone plan in [`docs/plan.md`](docs/plan.md)
- `cmake/`: vendored CPM.cmake and pinned dependency declarations
