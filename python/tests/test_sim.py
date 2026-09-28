"""Backtests against the simulated venue: latency, fills, fees, positions, and replay."""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path
from typing import Any

import jarvis
from jarvis import Cadence, Strategy, log
from jarvis import model as m
from jarvis.node import Node

IID = "BTCUSDT-PERP.BINANCE"
DAY = 1_788_220_800_000_000_000  # 2026-09-01T00:00:00Z
MS = 1_000_000


def _perpetual() -> m.CryptoPerpetual:
    usdt = m.Currency.from_str("USDT")
    return m.CryptoPerpetual(
        id=m.InstrumentId.from_str(IID),
        raw_symbol=m.Symbol("BTCUSDT"),
        base_currency=m.Currency.from_str("BTC"),
        quote_currency=usdt,
        settlement_currency=usdt,
        price_precision=1,
        size_precision=3,
        price_increment=m.Price("0.1"),
        size_increment=m.Quantity("0.001"),
        multiplier=m.Quantity("1"),
        margin_init=Decimal("0.05"),
        margin_maint=Decimal("0.025"),
        ts_event=DAY,
        ts_init=DAY,
    )


def _quote(ts: int, bid: str, ask: str) -> m.QuoteTick:
    return m.QuoteTick(
        instrument_id=m.InstrumentId.from_str(IID),
        bid_price=m.Price(bid),
        ask_price=m.Price(ask),
        bid_size=m.Quantity("1.000"),
        ask_size=m.Quantity("1.000"),
        ts_event=ts,
        ts_init=ts,
    )


def _trade(ts: int, price: str, aggressor: m.AggressorSide) -> m.TradeTick:
    return m.TradeTick(
        instrument_id=m.InstrumentId.from_str(IID),
        price=m.Price(price),
        size=m.Quantity("0.500"),
        aggressor_side=aggressor,
        trade_id=m.TradeId(str(ts)),
        ts_event=ts,
        ts_init=ts,
    )


def _write(catalog: Path, stream: str, source: int, events: list[Any]) -> None:
    with log.EventLogWriter(str(catalog / IID / stream / "2026-09-01")) as writer:
        for seq, event in enumerate(events, start=1):
            writer.append(event, seq=seq, ts=event.ts_init, source_id=source)


def _config(tmp_path: Path, sim: str) -> Path:
    catalog = tmp_path / "catalog"
    _write(catalog, "instrument", 9, [_perpetual()])
    quotes = [_quote(DAY + i * 100 * MS, "65000.0", "65000.1") for i in range(1, 30)]
    trades = [_trade(DAY + 1050 * MS, "64999.9", m.AggressorSide.SELL)]
    _write(catalog, "bookTicker", 2, quotes)
    _write(catalog, "aggTrade", 1, trades)
    path = tmp_path / "node.toml"
    path.write_text(
        f"""
[node]
id = "sim01"
seed = 3

[data]
catalog = "{catalog}"

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["{IID}"]
streams = ["aggTrade", "bookTicker"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"

[venues.sim]
{sim}

[[strategies]]
id = "taker-001"
impl = "py:Taker"
"""
    )
    return path


class Taker(Strategy):
    """Buys at market on the first quote, rests a bid below on the second."""

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.quotes = 0
        self.events: list[str] = []
        self.position: Any = None

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(IID, Cadence.EVERY)

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        self.quotes += 1
        if self.quotes == 1:
            ctx.submit(ctx.market(IID, m.OrderSide.BUY, "0.010"))
        elif self.quotes == 2:
            ctx.submit(ctx.limit(IID, m.OrderSide.BUY, "0.010", "65000.0", post_only=True))
        elif self.quotes == 25:
            self.position = ctx.position(IID)

    def on_order_event(self, ctx: jarvis.Context, event: Any) -> None:
        name = type(event).__name__
        if isinstance(event, m.OrderFilled):
            name += f" {event.last_px} {event.liquidity_side.name} {event.commission}"
        self.events.append(f"{name}@{(ctx.now() - DAY) // MS}ms")


SIM = """
fill_model = "top_of_book"
latency = { feed_ns = 1000000, out_ns = 20000000, in_ns = 5000000, jitter_ns = 0 }
fee = { schedule = "binance_usdm_vip0" }
balances = ["10000 USDT"]
"""


def test_a_backtest_against_the_simulated_venue(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, SIM), out=tmp_path / "run")
    strategy = Taker(id="taker-001")
    result = node.add_strategy(strategy).run()
    assert result.strategy_errors == 0
    assert result.venue_answers == 4
    # quote at 100 ms is seen at 101; the market order reaches the venue at 121 and its answers
    # arrive at 126. The bid rests from 221; the 1050 ms trade through it fills it at 1055 + 1.
    assert strategy.events == [
        "OrderSubmitted@101ms",
        "OrderAccepted@126ms",
        "OrderFilled 65000.1 TAKER 0.32500050 USDT@126ms",
        "OrderSubmitted@201ms",
        "OrderAccepted@226ms",
        "OrderFilled 65000.0 MAKER 0.13000000 USDT@1055ms",
    ]
    position = strategy.position
    assert position.side == m.PositionSide.LONG
    assert str(position.quantity) == "0.020"
    assert str(position.avg_px_open) == "65000.050000000"

    kinds = [r.kind for r in log.read(result.directory)]
    assert kinds.count("CryptoPerpetual") == 1
    assert kinds.count("AccountState") == 1
    assert kinds.count("OrderFilled") == 2

    replay = Node.from_run(result.directory).add_strategy(Taker(id="taker-001")).replay()
    assert replay.divergence is None, str(replay)
    assert replay.outputs == result.outputs


def test_unknown_fee_schedules_are_reported(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, 'fee = { schedule = "vip9" }'), out=tmp_path / "run")
    try:
        node.add_strategy(Taker(id="taker-001")).run()
    except Exception as e:  # noqa: BLE001 - the node reports configuration errors as exceptions
        assert "vip9" in str(e)
    else:
        raise AssertionError("an unknown fee schedule must stop the run")


class ParentBuyer(Strategy):
    """Rests a passthrough parent bid on the first quote and watches it fill."""

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.parent: Any = None
        self.views: list[Any] = []
        self.children: list[Any] = []
        self.events: list[str] = []

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(IID, Cadence.EVERY)

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        if self.parent is None:
            intent = ctx.limit(IID, m.OrderSide.BUY, "0.010", "65000.0", post_only=True)
            self.parent = ctx.submit_parent(intent)
            view = ctx.parent(self.parent)
            self.views.append((view.algo, str(view.quantity), view.children, view.active))
            self.children = [o.parent_id for o in ctx.open_orders()]

    def on_order_event(self, ctx: jarvis.Context, event: Any) -> None:
        self.events.append(type(event).__name__)
        if isinstance(event, m.OrderFilled):
            self.views.append(ctx.parent(self.parent))


def test_a_passthrough_parent_order(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, SIM), out=tmp_path / "run")
    strategy = ParentBuyer(id="taker-001")
    result = node.add_strategy(strategy).run()
    assert result.strategy_errors == 0
    assert strategy.views[0] == ("passthrough", "0.010", 1, True)
    assert strategy.children == [strategy.parent]  # the child names its parent
    assert strategy.events == ["OrderSubmitted", "OrderAccepted", "OrderFilled"]
    assert strategy.views[-1] is None  # filled: the parent closed

    try:
        ctx_error = None
        Node(_config(tmp_path / "b", SIM), out=tmp_path / "run-b").add_strategy(
            _BadAlgo(id="taker-001")
        ).run()
    except Exception as e:  # noqa: BLE001
        ctx_error = e
    assert ctx_error is None  # the error is caught inside the callback below
    assert _BadAlgo.message and "unknown execution algorithm" in _BadAlgo.message


class RestThenTake(Strategy):
    """Buys 0.010 with passive_then_aggressive: rests 300 ms at the bid, then takes."""

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.parent: Any = None
        self.children: list[tuple[str, str, bool, str]] = []
        self.fills: list[str] = []

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(IID, Cadence.EVERY)

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        if self.parent is None:
            intent = ctx.market(IID, m.OrderSide.BUY, "0.010")
            self.parent = ctx.submit_parent(intent, algo="passive_then_aggressive",
                                            params=[300 * MS])

    def on_order_event(self, ctx: jarvis.Context, event: Any) -> None:
        if isinstance(event, m.OrderSubmitted):
            order = ctx.order(event.client_order_id)
            self.children.append(
                (str(order.price), str(order.quantity), order.post_only, str(order.time_in_force))
            )
        if isinstance(event, m.OrderFilled):
            self.fills.append(str(event.last_px))


def test_passive_then_aggressive_rests_then_takes(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, SIM), out=tmp_path / "run")
    strategy = RestThenTake(id="taker-001")
    result = node.add_strategy(strategy).run()
    assert result.strategy_errors == 0
    assert strategy.children == [
        ("65000.0", "0.010", True, "TimeInForce.GTC"),  # rests at the bid
        ("65000.1", "0.010", False, "TimeInForce.IOC"),  # then takes at the ask
    ]
    assert strategy.fills == ["65000.1"]
    replay = Node.from_run(result.directory).add_strategy(RestThenTake(id="taker-001")).replay()
    assert replay.divergence is None, str(replay)
    assert replay.outputs == result.outputs


class _BadAlgo(Strategy):
    message = ""

    def on_start(self, ctx: jarvis.Context) -> None:
        try:
            ctx.submit_parent(ctx.limit(IID, m.OrderSide.BUY, "0.010", "65000.0"), algo="twap")
        except ValueError as e:
            type(self).message = str(e)


def test_the_run_report_matches_the_kernel(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, SIM), out=tmp_path / "run")
    strategy = Taker(id="taker-001")
    result = node.add_strategy(strategy).run()
    report = result.report()
    assert report.book == "L1 (bookTicker) + trades (aggTrade)"
    assert report.fill_model == "top_of_book"
    assert report.fee_schedule == "binance_usdm_vip0"
    assert "latency feed 1.000 ms, out 20.000 ms, in 5.000 ms" in report.venue
    row = report.row("taker-001", IID)
    assert row is not None
    assert (row.fills, row.maker_fills, row.taker_fills) == (2, 1, 1)
    position = strategy.position  # read by the strategy after both fills
    assert str(row.position) == "0.020"
    assert row.avg_px_open == position.avg_px_open
    assert row.commission == position.commission
    assert str(row.commission) == "0.45500050 USDT"
    counts = report.orders["taker-001"]
    assert (counts.submitted, counts.accepted, counts.filled) == (2, 2, 2)
    assert [str(b) for b in report.starting_balances] == ["10000.00000000 USDT"]
    assert [str(b) for b in report.ending_balances] == ["9999.54499950 USDT"]
    assert "fills 2 (maker 1, taker 1)" in str(report)
