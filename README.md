# jarvis

`jarvis` is the buildable project scaffold for a deterministic, event-driven C++20
backtesting kernel exposed to Python through nanobind.

This repository intentionally contains no trading-domain implementation yet. The native module,
freestanding target, test target, and benchmark target exist only to prove that the toolchain is
wired correctly.

## Requirements

- CMake 3.25 or newer
- Ninja
- GCC 13+, Clang 17+, or AppleClang 15+
- Python 3.11 or newer
- `uv` and `just` for the documented local workflow
- clang-format and clang-tidy for linting

MSVC is intentionally unsupported because the planned kernel requires `__int128`.

## Local workflow

```sh
just bootstrap
just test
just lint
just bench
```

`just fp` currently builds both determinism presets but does not claim to compare fingerprints.
`just bench` validates the benchmark dependency and target but does not emit synthetic results.
Both gates become real when the first kernel state and benchmark are implemented.

The equivalent packaging check is:

```sh
uv venv --python 3.11 .venv
uv pip install --python .venv/bin/python --reinstall .
.venv/bin/python -c 'import jarvis; print(jarvis._core.__doc__)'
```

## Layout

- `jarvis/`: future header-only kernel modules; empty modules are retained with `.gitkeep`
- `python/`: minimal nanobind and Python package boundary
- `testkit/`: compile-only freestanding guard
- `tests/`: future C++ tests
- `benchmarks/`: future benchmarks
- `docs/`: deterministic-kernel constraints and C++ subset
- `cmake/`: vendored CPM.cmake and pinned dependency declarations
