"""The node: one entry point for backtest, sandbox and live (docs/architecture.md section 4).

A strategy file is a complete deployable unit::

    import jarvis

    class MyStrategy(jarvis.Strategy):
        def on_start(self, ctx): ctx.subscribe_trades("BTCUSDT-PERP.BINANCE")
        def on_trade(self, ctx, trade): ctx.record("last", trade.price.as_decimal())

    if __name__ == "__main__":
        jarvis.main(MyStrategy)

and runs as ``python my_strategy.py --config node.toml`` (a backtest, or with ``--env sandbox``
a session on the venue's market data against the simulated exchange, in a build with the live
shell) or replays a run with ``python my_strategy.py --replay RUN_DIR``. ``Node`` is the same
thing as an object.
"""

from __future__ import annotations

import dataclasses
import os
import sys
from collections.abc import Iterable, Mapping, Sequence
from typing import Any

from . import determinism
from . import log as _log
from ._core import node as _native
from .report import RunReport
from .strategy import Strategy

NativeSpec = _native.NativeSpec


@dataclasses.dataclass(frozen=True)
class StrategyStats:
    """Callback timing of one Python strategy (wall clock; not part of the run's outputs)."""

    id: str
    calls: int
    total_ns: int
    max_ns: int
    overruns: int
    escapes: int


@dataclasses.dataclass(frozen=True)
class RunResult:
    directory: str  # empty when persistence.mode = "none"
    data_events: int
    skipped: int
    inputs: int
    outputs: int
    batches: int
    timers: int
    strategy_errors: int
    venue_answers: int  # order events from the simulated venue ([venues.sim])
    halted: bool
    state: str
    left_open: int  # orders still open when the node stopped (sandbox and live shutdown)
    first_ts: int
    last_ts: int
    strategies: tuple[StrategyStats, ...]
    feed: Mapping[str, int] | None = None  # sandbox and live: the market data feed's counters
    venue: Mapping[str, int] | None = None  # live: the account and order entry's counters
    epoch: int | None = None  # live: the ClientOrderId epoch this run took
    warnings: tuple[str, ...] = ()  # live: what the startup checks warned about

    @property
    def fingerprint(self) -> str:
        """SHA-256 over every record of the run log (inputs and outputs)."""
        return _log.fingerprint(self.directory, "all")[0] if self.directory else ""

    def report(self) -> RunReport:
        """Fills, fees and PnL of the run, labelled with its data and fill model."""
        if not self.directory:
            raise ValueError("the run was not recorded (persistence.mode = \"none\")")
        return RunReport.from_run(self.directory)

    def __str__(self) -> str:
        lines = [
            f"run: {self.directory or '(not recorded)'}",
            f"inputs: {self.inputs} (data {self.data_events}, batches {self.batches}, "
            f"timers {self.timers}, strategy errors {self.strategy_errors}, "
            f"venue answers {self.venue_answers})",
            f"outputs: {self.outputs}",
            f"skipped: {self.skipped}",
            f"state: {self.state}" + (" (halted by a strategy error)" if self.halted else ""),
        ]
        if self.left_open:
            lines.append(f"left open at stop: {self.left_open} orders")
        if self.directory:
            digest, records = _log.fingerprint(self.directory, "all")
            lines.append(f"fingerprint: {digest} records={records}")
        if self.feed is not None:
            lines.append("feed: " + ", ".join(f"{k} {v}" for k, v in self.feed.items()))
        if self.venue is not None:
            lines.append("venue: " + ", ".join(f"{k} {v}" for k, v in self.venue.items()))
        if self.epoch is not None:
            lines.append(f"epoch: {self.epoch}")
        lines.extend(f"warning: {w}" for w in self.warnings)
        for s in self.strategies:
            mean = s.total_ns / s.calls / 1000 if s.calls else 0.0
            lines.append(
                f"strategy {s.id}: {s.calls} callbacks, mean {mean:.1f} us, "
                f"max {s.max_ns / 1000:.1f} us"
            )
        return "\n".join(lines)


@dataclasses.dataclass(frozen=True)
class Divergence:
    seq: int
    recorded: str  # what the run log holds
    replayed: str  # what the replay computed instead


@dataclasses.dataclass(frozen=True)
class ReplayReport:
    directory: str
    inputs: int
    outputs: int
    divergence: Divergence | None
    state: str

    def __str__(self) -> str:
        if self.divergence is not None:
            d = self.divergence
            text = (
                f"ReplayDivergence in {self.directory} at seq {d.seq}\n"
                f"  recorded: {d.recorded}\n  replayed: {d.replayed}"
            )
        else:
            text = (
                f"replayed {self.directory}: {self.inputs} inputs, {self.outputs} outputs, "
                "no divergence"
            )
        return text + ("\n" + self.state.rstrip("\n") if self.state else "")


class Node:
    """A node loaded from a configuration file, or from a run directory for replay."""

    def __init__(
        self,
        config: str | os.PathLike[str],
        *,
        env: str | None = None,
        sets: Iterable[str] = (),
        out: str | os.PathLike[str] | None = None,
    ) -> None:
        self._setup = _native.NodeSetup.load(os.fspath(config), env, list(sets))
        self._out = os.fspath(out) if out is not None else None
        self._strategies: list[Any] = []
        self._run_directory: str | None = None

    @classmethod
    def from_run(cls, directory: str | os.PathLike[str]) -> Node:
        """The node a run directory was recorded with (configuration and overrides)."""
        node = cls.__new__(cls)
        node._setup = _native.NodeSetup.load_run(os.fspath(directory))
        node._out = None
        node._strategies = []
        node._run_directory = os.fspath(directory)
        return node

    @property
    def node_id(self) -> str:
        return self._setup.node_id

    @property
    def env(self) -> str:
        return self._setup.env

    @property
    def seed(self) -> int:
        return self._setup.seed

    @property
    def config_hash(self) -> str:
        return self._setup.config_hash

    @property
    def strategy_entries(self) -> list[dict[str, Any]]:
        """The configuration's [[strategies]] entries: id, impl, instruments, params."""
        return self._setup.strategies

    @property
    def strategies(self) -> list[Any]:
        return list(self._strategies)

    def add_strategy(self, strategy: Strategy | Any) -> Node:
        """Adds a Python strategy object; the node calls the callbacks it defines."""
        self._strategies.append(strategy)
        return self

    def add_native_strategy(
        self,
        name: str,
        params: Mapping[str, Any] | None = None,
        *,
        id: str | None = None,  # noqa: A002
    ) -> Node:
        """Adds a C++ strategy registered in this build under ``name``."""
        self._strategies.append(NativeSpec(name, id or f"{name.lower()}-001", dict(params or {})))
        return self

    def run(self, *, run_for: float | None = None) -> RunResult:
        """Runs the node and writes the run directory: a backtest over its [data], or a sandbox
        or live session until SIGINT, SIGTERM or `run_for` seconds."""
        if not self._strategies:
            raise ValueError("the node has no strategies; add_strategy() first")
        with self._guard():
            summary = self._setup.run(self._strategies, self._out, run_for)
        stats = tuple(StrategyStats(**s) for s in summary.pop("strategies"))
        if "warnings" in summary:
            summary["warnings"] = tuple(summary["warnings"])
        result = RunResult(strategies=stats, **summary)
        self._run_directory = result.directory or None
        return result

    def replay(
        self,
        directory: str | os.PathLike[str] | None = None,
        *,
        until: int | None = None,
        dump_state: bool = False,
    ) -> ReplayReport:
        """Recomputes a run's outputs from its inputs and reports the first divergence."""
        target = os.fspath(directory) if directory is not None else self._run_directory
        if not target:
            raise ValueError("replay needs a run directory")
        if not self._strategies:
            raise ValueError("the node has no strategies; add_strategy() first")
        with self._guard():
            report = self._setup.replay(target, self._strategies, until, dump_state)
        divergence = report["divergence"]
        return ReplayReport(
            directory=target,
            inputs=report["inputs"],
            outputs=report["outputs"],
            divergence=Divergence(**divergence) if divergence is not None else None,
            state=report["state"],
        )

    def _guard(self) -> Any:
        if not self._setup.strict_determinism:
            return _NullContext()
        for strategy in self._strategies:
            module = sys.modules.get(type(strategy).__module__)
            if module is not None:
                determinism.scrub_module(module)
        return determinism.guard()


class _NullContext:
    def __enter__(self) -> None:
        return None

    def __exit__(self, *exc: object) -> None:
        return None


def build_strategies(
    entries: Sequence[Mapping[str, Any]], classes: Sequence[type]
) -> list[Any]:
    """Strategy objects for the configuration's entries: ``py:<Class>`` entries use the class
    of that name from `classes`, ``cpp:<name>`` entries the registered C++ strategy. Without
    entries, one instance of each class with its default parameters."""
    if not entries:
        return [cls(id=f"{cls.__name__.lower()}-{i + 1:03d}") for i, cls in enumerate(classes)]
    by_name = {cls.__name__: cls for cls in classes}
    out: list[Any] = []
    for entry in entries:
        impl = entry["impl"]
        if impl.startswith("py:"):
            name = impl[3:]
            cls = by_name.get(name)
            if cls is None:
                known = ", ".join(sorted(by_name)) or "none"
                raise ValueError(
                    f"strategy {entry['id']}: impl = {impl!r} but the program defines {known}"
                )
            out.append(cls(entry["params"], id=entry["id"], instruments=entry["instruments"]))
        elif impl.startswith("cpp:"):
            out.append(NativeSpec(impl[4:], entry["id"], entry["params"]))
        else:
            raise ValueError(f"strategy {entry['id']}: impl must be py:<class> or cpp:<name>")
    return out


def run_main(classes: Sequence[type], argv: Sequence[str] | None = None) -> int:
    """``jarvis.main`` without the exit: returns the exit code (0, 1 failure, 2 usage,
    3 replay divergence)."""
    program = os.path.basename(sys.argv[0]) if sys.argv and sys.argv[0] else "jarvis"
    args = list(sys.argv[1:] if argv is None else argv)
    try:
        parsed = _native.parse_args(args)
    except ValueError as e:
        print(f"{program}: {e}\n{_native.usage(program)}", file=sys.stderr)
        return 2
    if parsed["help"]:
        print(_native.usage(program))
        return 0
    try:
        if parsed["replay"]:
            node = Node.from_run(parsed["replay"])
        else:
            node = Node(
                parsed["config"], env=parsed["env"], sets=parsed["sets"], out=parsed["out"] or None
            )
        if argv is None:
            # Same PYTHONHASHSEED for the same node seed; re-executes when it is missing.
            determinism.ensure_hash_seed(node.seed)
        for strategy in build_strategies(node.strategy_entries, classes):
            node._strategies.append(strategy)
        if parsed["replay"]:
            report = node.replay(until=parsed["until"], dump_state=parsed["dump_state"])
            print(report)
            return 3 if report.divergence is not None else 0
        result = node.run(run_for=parsed["run_for"])
    except (ValueError, OSError) as e:
        print(f"{program}: {e}", file=sys.stderr)
        return 1
    print(result)
    return 1 if result.halted else 0


def main(*classes: type, argv: Sequence[str] | None = None) -> None:
    """The single-file entry: parses the node command line and exits with its status."""
    sys.exit(run_main(classes, argv))


__all__ = [
    "Divergence",
    "NativeSpec",
    "Node",
    "ReplayReport",
    "RunResult",
    "StrategyStats",
    "build_strategies",
    "main",
    "run_main",
]
