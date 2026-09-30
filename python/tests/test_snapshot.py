"""EngineState snapshots of Python nodes: on_save and on_load, replay checking every snapshot, and
replay from each snapshot to the end of the run (docs/architecture.md section 16.3)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jarvis
from jarvis import Cadence, Strategy
from jarvis import model as m
from jarvis.node import Node

from test_sim import IID, MS, SIM, _config


class Stateful(Strategy):
    """Quotes a post-only bid every other quote, cancels on the next, buys at market every
    seventh, and runs a passive_then_aggressive parent from the third: its counters and ids are
    its own state, saved and restored as JSON."""

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.quotes = 0
        self.resting: str | None = None
        self.parent: str | None = None
        self.log: list[str] = []

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(IID, Cadence.EVERY)

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        self.quotes += 1
        if self.quotes == 3:
            intent = ctx.market(IID, m.OrderSide.BUY, "0.010")
            self.parent = str(
                ctx.submit_parent(intent, algo="passive_then_aggressive", params=[400 * MS])
            )
        if self.resting is None and self.quotes % 2 == 0:
            self.resting = str(
                ctx.submit(ctx.limit(IID, m.OrderSide.BUY, "0.010", "64999.0", post_only=True))
            )
        elif self.resting is not None and self.quotes % 2 == 1:
            ctx.cancel(m.ClientOrderId(self.resting))
            self.resting = None
        if self.quotes % 7 == 0:
            ctx.submit(ctx.market(IID, m.OrderSide.BUY, "0.001"))

    def on_order_event(self, ctx: jarvis.Context, event: Any) -> None:
        self.log.append(f"{type(event).__name__} {event.client_order_id}@{ctx.seq()}")

    def on_save(self) -> bytes:
        return json.dumps(
            {"quotes": self.quotes, "resting": self.resting, "parent": self.parent},
            sort_keys=True,
        ).encode()

    def on_load(self, state: bytes) -> None:
        saved = json.loads(state)
        self.quotes = saved["quotes"]
        self.resting = saved["resting"]
        self.parent = saved["parent"]


class Forgetful(Stateful):
    """The same strategy without on_save and on_load."""

    on_save = None  # type: ignore[assignment]
    on_load = None  # type: ignore[assignment]


def _run(tmp_path: Path, cls: type, every: int) -> Any:
    config = _config(tmp_path, SIM)
    text = config.read_text().replace('impl = "py:Taker"', f'impl = "py:{cls.__name__}"')
    config.write_text(text)
    node = Node(config, out=tmp_path / "run", sets=[f"persistence.snapshot_every={every}"])
    return node.add_strategy(cls(id="taker-001")).run()


def test_snapshots_match_the_replay_and_replay_from_each_reaches_the_same_end(
    tmp_path: Path,
) -> None:
    result = _run(tmp_path, Stateful, 7)
    assert result.strategy_errors == 0
    snapshots = sorted(Path(result.directory).glob("snapshot-*.jsnap"))
    assert result.snapshots == len(snapshots) > 3
    assert result.snapshot_failures == 0

    full = Node.from_run(result.directory).add_strategy(Stateful(id="taker-001")).replay()
    assert full.divergence is None, str(full)
    assert full.snapshots_checked == len(snapshots)

    for snapshot in snapshots:
        strategy = Stateful(id="taker-001")
        replay = (
            Node.from_run(result.directory)
            .add_strategy(strategy)
            .replay(from_snapshot=snapshot)
        )
        assert replay.divergence is None, f"{snapshot.name}: {replay}"
        assert replay.start_seq == int(snapshot.stem.split("-")[1])
        assert replay.inputs == result.inputs - replay.start_seq
        assert replay.outputs <= result.outputs


def test_a_strategy_without_on_save_makes_incomplete_snapshots(tmp_path: Path) -> None:
    result = _run(tmp_path, Forgetful, 7)
    snapshots = sorted(Path(result.directory).glob("snapshot-*.jsnap"))
    assert len(snapshots) > 3
    # The whole run still replays, snapshots included; starting from one is refused.
    full = Node.from_run(result.directory).add_strategy(Forgetful(id="taker-001")).replay()
    assert full.divergence is None and full.snapshots_checked == len(snapshots)
    try:
        Node.from_run(result.directory).add_strategy(Forgetful(id="taker-001")).replay(
            from_snapshot=snapshots[0]
        )
    except ValueError as e:
        assert "does not describe" in str(e)
    else:
        raise AssertionError("an incomplete snapshot must not be replayed from")


def test_a_changed_strategy_state_is_a_divergence_at_the_snapshot(tmp_path: Path) -> None:
    result = _run(tmp_path, Stateful, 7)

    class Drifting(Stateful):
        def on_save(self) -> bytes:
            return super().on_save() + b" "  # a different state for the same inputs

    Drifting.__name__ = "Stateful"
    replay = Node.from_run(result.directory).add_strategy(Drifting(id="taker-001")).replay()
    assert replay.divergence is not None
    assert "snapshot-" in replay.divergence.recorded
