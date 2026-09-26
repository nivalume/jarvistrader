"""Python strategies on the node: run, replay, failures, guards, batches and main()."""

from __future__ import annotations

import time
from decimal import Decimal
from pathlib import Path
from typing import Any

import pytest

import jarvis
from jarvis import Cadence, Strategy, features, log
from jarvis import model as m
from jarvis.node import Node, run_main

IID = "BTCUSDT-PERP.BINANCE"
DAY = 1_788_220_800_000_000_000  # 2026-09-01T00:00:00Z
SECOND = 1_000_000_000


def _trade(ts: int, price: str) -> m.TradeTick:
    return m.TradeTick(
        instrument_id=m.InstrumentId.from_str(IID),
        price=m.Price(price),
        size=m.Quantity("0.010"),
        aggressor_side=m.AggressorSide.BUY,
        trade_id=m.TradeId(str(ts)),
        ts_event=ts,
        ts_init=ts,
    )


def _quote(ts: int, bid: str, ask: str) -> m.QuoteTick:
    return m.QuoteTick(
        instrument_id=m.InstrumentId.from_str(IID),
        bid_price=m.Price(bid),
        ask_price=m.Price(ask),
        bid_size=m.Quantity("1.000"),
        ask_size=m.Quantity("2.000"),
        ts_event=ts,
        ts_init=ts,
    )


def _write(catalog: Path, stream: str, source: int, events: list[Any]) -> None:
    with log.EventLogWriter(str(catalog / IID / stream / "2026-09-01")) as writer:
        for seq, event in enumerate(events, start=1):
            writer.append(event, seq=seq, ts=event.ts_init, source_id=source)


def _config(tmp_path: Path, strategies: str = "", extra: str = "") -> Path:
    catalog = tmp_path / "catalog"
    trades = [_trade(DAY + i * SECOND // 2, f"65000.{i % 10}") for i in range(1, 41)]
    quotes = [_quote(DAY + i * SECOND, f"64999.{i % 10}", f"65001.{i % 10}") for i in range(1, 21)]
    _write(catalog, "aggTrade", 1, trades)
    _write(catalog, "bookTicker", 2, quotes)
    path = tmp_path / "node.toml"
    path.write_text(
        f"""
[node]
id = "py01"
seed = 11

[data]
catalog = "{catalog}"

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["{IID}"]
streams = ["aggTrade", "bookTicker"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"
{strategies}
{extra}
"""
    )
    return path


class Recorder(Strategy):
    """Exercises subscriptions, features, timers, books and records."""

    params = {"timer_s": 5, "shift": 0}

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.calls: list[str] = []
        self.ema_id = -1

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_trades(IID)
        ctx.subscribe_quotes(IID, Cadence.CONFLATED)
        ctx.subscribe_book(IID, Cadence.CONFLATED, m.BookType.L1_MBP)
        self.ema_id = ctx.feature(features.ema(IID, 4), Cadence.sampled_ms(2000))
        period = self.params["timer_s"] * SECOND
        ctx.set_timer(7, ctx.now() + period, period)
        self.calls.append(f"start@{ctx.now()}")

    def on_trade(self, ctx: jarvis.Context, trade: m.TradeTick) -> None:
        ctx.record("px", trade.price.as_decimal() + self.params["shift"])

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        self.calls.append(f"quote@{quote.ts_init}")

    def on_book(self, ctx: jarvis.Context, book: Any) -> None:
        bid = book.best_bid()
        assert bid is not None
        ctx.record("bid", bid[0].as_decimal())

    def on_feature(self, ctx: jarvis.Context, feature_id: int, value: Decimal, ts: int) -> None:
        assert feature_id == self.ema_id
        ctx.record("ema", value)

    def on_timer(self, ctx: jarvis.Context, timer_id: int, deadline: int) -> None:
        self.calls.append(f"timer{timer_id}@{deadline - DAY}")
        ctx.record("rng", ctx.rng(1) % 1000)

    def on_stop(self, ctx: jarvis.Context) -> None:
        self.calls.append("stop")


ENTRY = """
[[strategies]]
id = "rec-001"
impl = "py:Recorder"
params = { timer_s = 4 }
"""


def test_run_then_replay_reproduces_every_output(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, ENTRY), out=tmp_path / "run")
    strategy = Recorder(node.strategy_entries[0]["params"], id="rec-001")
    result = node.add_strategy(strategy).run()
    assert result.state == "STOPPED"
    assert result.data_events == 60
    assert result.strategy_errors == 0
    assert result.timers == 4  # every 4 s from the first event at 0.5 s until 20 s
    assert strategy.calls[0] == f"start@{DAY + SECOND // 2}"
    assert strategy.calls[-1] == "stop"
    assert "timer7@4500000000" in strategy.calls
    assert result.outputs > 60
    assert result.strategies[0].calls > 60

    kinds = {r.kind for r in log.read(result.directory)}
    assert {"StrategyRecord", "FeatureUpdate", "TimerFired", "BatchEnd"} <= kinds

    replay = Node.from_run(result.directory)
    report = replay.add_strategy(Recorder({"timer_s": 4}, id="rec-001")).replay()
    assert report.divergence is None, str(report)
    assert report.inputs == result.inputs
    assert report.outputs == result.outputs

    changed = Node.from_run(result.directory)
    report = changed.add_strategy(Recorder({"timer_s": 4, "shift": 1}, id="rec-001")).replay()
    assert report.divergence is not None
    assert "StrategyRecord" in report.divergence.recorded
    assert report.divergence.recorded != report.divergence.replayed


def test_the_same_run_twice_writes_the_same_log(tmp_path: Path) -> None:
    config = _config(tmp_path, ENTRY)
    a = Node(config, out=tmp_path / "a").add_strategy(Recorder({"timer_s": 4})).run()
    b = Node(config, out=tmp_path / "b").add_strategy(Recorder({"timer_s": 4})).run()
    assert a.fingerprint == b.fingerprint
    equal, compared, _, _ = log.compare(a.directory, b.directory, "all")
    assert equal and compared == a.inputs + a.outputs


class Failing(Strategy):
    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.trades = 0
        self.errors: list[Any] = []

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_trades(IID)

    def on_trade(self, ctx: jarvis.Context, trade: m.TradeTick) -> None:
        self.trades += 1
        if self.trades == 3:
            raise ZeroDivisionError("boom")

    def on_error(self, ctx: jarvis.Context, error: Any) -> None:
        self.errors.append(error)


def test_an_exception_becomes_a_strategy_error_and_halts_the_strategy(
    tmp_path: Path, capfd: pytest.CaptureFixture[str]
) -> None:
    strategy = Failing(id="fail-001")
    result = Node(_config(tmp_path), out=tmp_path / "run").add_strategy(strategy).run()
    assert result.strategy_errors == 1
    assert strategy.trades == 3  # halted: no trades after the failure
    assert len(strategy.errors) == 1
    assert strategy.errors[0].kind == m.StrategyErrorKind.EXCEPTION
    assert "ZeroDivisionError" in capfd.readouterr().err
    errors = [r for r in log.read(result.directory) if r.kind == "StrategyError"]
    assert len(errors) == 1

    report = Node.from_run(result.directory).add_strategy(Failing(id="fail-001")).replay()
    assert report.divergence is None, str(report)


def test_halt_node_stops_the_run(tmp_path: Path) -> None:
    config = _config(tmp_path, extra='[risk]\non_strategy_error = "halt_node"\n')
    result = Node(config, out=tmp_path / "run").add_strategy(Failing()).run()
    assert result.halted
    assert result.data_events < 60


class Clocky(Strategy):
    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_trades(IID)

    def on_trade(self, ctx: jarvis.Context, trade: m.TradeTick) -> None:
        time.time()


def test_the_wall_clock_is_refused_inside_callbacks(
    tmp_path: Path, capfd: pytest.CaptureFixture[str]
) -> None:
    result = Node(_config(tmp_path), out=tmp_path / "run").add_strategy(Clocky()).run()
    assert result.strategy_errors == 1
    assert "NondeterminismError" in capfd.readouterr().err
    time.time()  # outside the run the clock works


class Keeper(Strategy):
    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.ctx: Any = None

    def on_start(self, ctx: jarvis.Context) -> None:
        self.ctx = ctx
        ctx.subscribe_trades(IID)

    def on_trade(self, ctx: jarvis.Context, trade: m.TradeTick) -> None:
        ctx.record("n", 1)


def test_a_context_kept_after_its_callback_is_refused(tmp_path: Path) -> None:
    strategy = Keeper()
    Node(_config(tmp_path), out=tmp_path / "run").add_strategy(strategy).run()
    with pytest.raises(TypeError, match="only valid during the callback"):
        strategy.ctx.now()


class Batches(Strategy):
    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.sizes: list[int] = []
        self.kept: Any = None

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_trades(IID, Cadence.ON_BATCH)

    def on_trade_batch(self, ctx: jarvis.Context, batch: Any) -> None:
        self.sizes.append(len(batch))
        prices = batch.price_raw
        assert prices.dtype.name == "int64" and not prices.flags.writeable
        ctx.record("sum", int(prices.sum()) // 1_000_000_000)
        if self.params.get("keep") and self.kept is None:
            self.kept = prices[:1]


def test_on_trade_batch_gets_read_only_columns(tmp_path: Path) -> None:
    strategy = Batches()
    result = Node(_config(tmp_path), out=tmp_path / "run").add_strategy(strategy).run()
    assert result.strategy_errors == 0
    assert sum(strategy.sizes) == 40
    assert all(size == 1 for size in strategy.sizes)  # one trade per timestamp in this data


def test_a_batch_array_kept_after_the_callback_is_reported(
    tmp_path: Path, capfd: pytest.CaptureFixture[str]
) -> None:
    strategy = Batches({"keep": True})
    result = Node(_config(tmp_path), out=tmp_path / "run").add_strategy(strategy).run()
    assert result.strategy_errors == 1
    assert result.strategies[0].escapes == 1
    assert "kept a batch" in capfd.readouterr().err


def test_main_runs_and_replays(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    config = _config(tmp_path, ENTRY)
    out = tmp_path / "run"
    assert run_main([Recorder], ["--config", str(config), "--out", str(out)]) == 0
    assert "fingerprint:" in capsys.readouterr().out
    assert run_main([Recorder], ["--replay", str(out), "--until", "20", "--dump-state"]) == 0
    text = capsys.readouterr().out
    assert "no divergence" in text and "at seq=20" in text
    assert run_main([Recorder], ["--replay", str(out), "--out", "x"]) == 2
    assert run_main([], ["--config", str(config), "--out", str(tmp_path / "r2")]) == 1
    assert "defines none" in capsys.readouterr().err


def test_strategy_params_merge_config_over_class_defaults(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, ENTRY))
    [strategy] = jarvis.node.build_strategies(node.strategy_entries, [Recorder])
    assert strategy.params == {"timer_s": 4, "shift": 0}
    assert strategy.id == "rec-001"


def test_unknown_native_strategies_are_reported(tmp_path: Path) -> None:
    node = Node(_config(tmp_path), out=tmp_path / "run").add_native_strategy("NoSuchThing")
    with pytest.raises(ValueError, match="no C\\+\\+ strategy named 'NoSuchThing'"):
        node.run()
