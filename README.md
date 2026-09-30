# jarvis

`jarvis` is a deterministic, event-driven C++20 trading kernel for Binance with a Python strategy
layer (nanobind). The design is in [`docs/architecture.md`](docs/architecture.md), the
milestones in [`docs/plan.md`](docs/plan.md), and running nodes in
[`docs/runbook.md`](docs/runbook.md).

Status: milestones M0 to M4 are complete: engineering gates, the core and model, the engine and
data layer, order execution, the simulated exchange and risk, and the Binance USDⓈ-M adapter with
sandbox runs. M5 (live trading, reconciliation, persistence and recovery, telemetry, admin,
execution algorithms, thread placement) is implemented and tested against scripted and
simulated venues; its acceptance runs on the Binance testnet (72 hours, chaos, backward trace
validation from live logs) need API keys and are still open.

A strategy is one file that runs as a node:

```python
import jarvis
from jarvis import Cadence


class MidLogger(jarvis.Strategy):
    def on_start(self, ctx):
        ctx.subscribe_quotes("BTCUSDT-PERP.BINANCE", Cadence.sampled_ms(1000))

    def on_quote(self, ctx, quote):
        ctx.record("bid", quote.bid_price.as_decimal())


if __name__ == "__main__":
    jarvis.main(MidLogger)
```

```sh
# one day of Binance data into a catalog of event logs
python -m jarvis.data binance-vision aggTrades --symbol BTCUSDT --start 2024-03-30 --catalog catalog
python -m jarvis.data binance-vision bookTicker --symbol BTCUSDT --start 2024-03-30 --catalog catalog
# a backtest writes a run directory; replay recomputes every output and reports a divergence
python examples/py/trade_logger.py --config examples/config/trade_logger.toml --out runs/tlog
python examples/py/trade_logger.py --replay runs/tlog
```

The C++ twin, `examples/cpp/trade_logger.cpp`, is the same strategy behind
`jarvis::node_main<TradeLogger>` and writes a byte-identical run log (`build/rel/bin/trade_logger`
takes the same arguments).

Orders run against the simulated venue once the catalog holds the instrument's definition:

```sh
python -m jarvis.data binance-instrument --symbol BTCUSDT --day 2024-03-30 --catalog catalog \
    --tick 0.10 --step 0.001 --min-qty 0.001 --max-qty 1000 --min-notional 100
python examples/py/mm_quote.py --config examples/config/mm_quote.toml --out runs/mm
build/rel/bin/jarvis report runs/mm    # fills, fees and PnL, labelled with data and fill model
```

`examples/cpp/pegged_mm.cpp` is the C++ twin of the market maker. Event logs remain readable
from Python:

```python
from jarvis import log

for record in log.read("runs/tlog"):
    print(record.seq, record.kind, record.event)
print(log.fingerprint("runs/tlog", "all"))
```

The `jarvis` command-line tool (built with the `dev`, `rel` and `det-o0` presets under
`build/<preset>/bin/`) writes the deterministic corpus, fingerprints and compares logs, dumps them
as text, round-trips model strings, checks node configurations and replays runs of registered
C++ strategies:

```sh
build/rel/bin/jarvis corpus --seed 7 --events 200000 --out runs/corpus
build/rel/bin/jarvis fingerprint runs/corpus
build/rel/bin/jarvis dump runs/tlog --limit 20
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
event logs), `just zero-alloc`, `just layering`, `just bench`, `just bench-py`,
`just bench-compare base=main`, `just tla`, `just tla-changed`,
`just fuzz target=decimal|wire|config|smoke`, `just tsan`. `tools/m2_acceptance.sh` repeats the
M2 acceptance run on one day of Binance data.
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
  (`node`, `network`, `adapter`, `live`); modules that have not landed yet keep a `.gitkeep`
- `python/`: the `jarvis` package (`jarvis.model`, `jarvis.log`, `jarvis.determinism`,
  `jarvis.strategy`, `jarvis.features`, `jarvis.node`, `jarvis.data`) and its nanobind sources
- `examples/`: example strategies in Python and C++ with their configurations (for tests and
  soak runs; not part of the wheel)
- `testkit/`: property-test generator, zero-allocation counter, doctest entry point, freestanding guard
- `tests/`: C++ tests per layer, golden cases, fuzz targets with seed corpora, the nautilus
  conformance snapshot, and pytest for the tools
- `benchmarks/`: gating (`hot/`) and report-only (`report/`) benchmarks with `thresholds.toml`
- `specs/`: TLA+ specs and `specs/tla/MAP.toml`
- `tools/`: layering checker, golden runner, benchmark A/B, TLA+ runner and spec selector,
  milestone acceptance scripts
- `docs/`: deterministic-kernel constraints and C++ subset; the system design is in
  [`docs/architecture.md`](docs/architecture.md), the milestone plan in [`docs/plan.md`](docs/plan.md)
  and the operations runbook in [`docs/runbook.md`](docs/runbook.md)
- `cmake/`: vendored CPM.cmake and pinned dependency declarations
