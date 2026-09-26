# jarvis

`jarvis` is a deterministic, event-driven C++20 trading kernel for Binance with a Python strategy
layer (nanobind). The design is in [`docs/architecture.md`](docs/architecture.md) and the
milestones in [`docs/plan.md`](docs/plan.md).

Status: milestones M0 (engineering gates and test harness) and M1 (core, the nautilus-compatible
model, the event log, typed node configuration, the lifecycle state machine and the Python
bindings) are complete. The engine, strategy hosting and replay arrive in M2.

```python
from jarvis import log, model

tick = model.TradeTick(
    instrument_id=model.InstrumentId.from_str("BTCUSDT-PERP.BINANCE"),
    price=model.Price("65000.1"),
    size=model.Quantity("0.010"),
    aggressor_side=model.AggressorSide.BUY,
    trade_id=model.TradeId("1"),
    ts_event=1_767_225_600_000_000_000,
    ts_init=1_767_225_600_000_000_000,
)
with log.EventLogWriter("runs/demo") as writer:
    writer.append(tick, seq=1, ts=tick.ts_init)
print(log.fingerprint("runs/demo"))
```

The `jarvis` command-line tool (built with the `dev`, `rel` and `det-o0` presets under
`build/<preset>/bin/`) writes the deterministic corpus, fingerprints and compares logs, dumps them
as text, round-trips model strings and checks node configurations:

```sh
build/rel/bin/jarvis corpus --seed 7 --events 200000 --out runs/corpus
build/rel/bin/jarvis fingerprint runs/corpus
build/rel/bin/jarvis dump runs/corpus --limit 20
build/rel/bin/jarvis config examples/config/node.toml --env sandbox --set node.seed=7
```

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

Individual tiers: `just test`, `just golden`, `just fp` (Release and `-O0` must write identical
event logs), `just zero-alloc`, `just layering`, `just bench`, `just bench-compare base=main`,
`just tla`, `just tla-changed`, `just fuzz target=decimal|wire|config|smoke`, `just tsan`.
Recipes whose feature has not landed yet print `SKIPPED` and name the milestone in
`docs/plan.md` that adds it. The gate order and what each tier checks are described in
`docs/architecture.md` section 17.

The equivalent packaging check is:

```sh
uv venv --python 3.11 .venv
uv pip install --python .venv/bin/python --reinstall .
.venv/bin/python -c 'import jarvis; print(jarvis.build_info())'
```

## Layout

- `jarvis/`: header-only kernel layers (`core`, `model`, `engine`, ...) and the `jarvis_shell` layers
  (`node`, `network`, `adapter`, `live`); empty modules are retained with `.gitkeep`
- `python/`: the `jarvis` package (`jarvis.model`, `jarvis.log`, `jarvis.determinism`) and its
  nanobind sources
- `examples/`: example configuration (strategies arrive with M2)
- `testkit/`: property-test generator, zero-allocation counter, doctest entry point, freestanding guard
- `tests/`: C++ tests per layer, golden cases, fuzz targets with seed corpora, the nautilus
  conformance snapshot, and pytest for the tools
- `benchmarks/`: gating (`hot/`) and report-only (`report/`) benchmarks with `thresholds.toml`
- `specs/`: TLA+ specs and `specs/tla/MAP.toml`
- `tools/`: layering checker, golden runner, benchmark A/B, TLA+ runner and spec selector
- `docs/`: deterministic-kernel constraints and C++ subset; the system design is in
  [`docs/architecture.md`](docs/architecture.md) and the milestone plan in [`docs/plan.md`](docs/plan.md)
- `cmake/`: vendored CPM.cmake and pinned dependency declarations
