"""Orders from Python strategies: submit, denial, cancel, queries, and replay of all of it."""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path
from typing import Any

import jarvis
from jarvis import Strategy, log
from jarvis import model as m
from jarvis.node import Node

IID = "BTCUSDT-PERP.BINANCE"
DAY = 1_788_220_800_000_000_000  # 2026-09-01T00:00:00Z
SECOND = 1_000_000_000


def _perpetual(ts: int) -> m.CryptoPerpetual:
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
        ts_event=ts,
        ts_init=ts,
    )


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


def _config(tmp_path: Path, define: bool = True) -> Path:
    catalog = tmp_path / "catalog"
    events: list[Any] = [_perpetual(DAY)] if define else []
    events += [_trade(DAY + i * SECOND // 2, f"65000.{i % 10}") for i in range(1, 11)]
    with log.EventLogWriter(str(catalog / IID / "aggTrade" / "2026-09-01")) as writer:
        for seq, event in enumerate(events, start=1):
            writer.append(event, seq=seq, ts=event.ts_init, source_id=1)
    path = tmp_path / "node.toml"
    path.write_text(
        f"""
[node]
id = "ord01"
seed = 5

[data]
catalog = "{catalog}"

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["{IID}"]
streams = ["aggTrade"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"

[[strategies]]
id = "quoter-001"
impl = "py:Quoter"
"""
    )
    return path


class Quoter(Strategy):
    """Submits on the first trade, inspects on the third, cancels on the fifth."""

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.trades = 0
        self.events: list[str] = []
        self.cid: Any = None
        self.checks: dict[str, Any] = {}

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_trades(IID)

    def on_trade(self, ctx: jarvis.Context, trade: m.TradeTick) -> None:
        self.trades += 1
        if self.trades == 1:
            intent = ctx.limit(IID, m.OrderSide.BUY, "0.010", "64000.0", post_only=True)
            self.cid = ctx.submit(intent)
            ctx.submit(ctx.limit(IID, m.OrderSide.BUY, "0.010", "64000.05"))
        elif self.trades == 3:
            order = ctx.order(self.cid)
            self.checks["status"] = order.status
            self.checks["leaves"] = str(order.leaves_qty)
            self.checks["open"] = len(ctx.open_orders())
            definition = ctx.instrument(IID)
            self.checks["tick"] = None if definition is None else str(definition.price_increment)
        elif self.trades == 5:
            self.checks["cancel"] = ctx.cancel(self.cid)
            self.checks["cancel_again"] = ctx.cancel(self.cid)
            self.checks["modify"] = ctx.modify(self.cid, price="63990.0")
            self.checks["cancel_all"] = ctx.cancel_all()
            self.checks["still_open"] = len(ctx.open_orders(IID))

    def on_order_event(self, ctx: jarvis.Context, event: Any) -> None:
        name = type(event).__name__
        if isinstance(event, m.OrderDenied):
            name += f" {event.reason}"
        self.events.append(name)


def test_orders_from_python_run_and_replay(tmp_path: Path) -> None:
    node = Node(_config(tmp_path), out=tmp_path / "run")
    strategy = Quoter(id="quoter-001")
    result = node.add_strategy(strategy).run()
    assert result.strategy_errors == 0
    assert strategy.events == [
        "OrderSubmitted",
        "OrderDenied PRICE_INVALID_PRECISION",
        "OrderPendingCancel",
    ]
    assert str(strategy.cid) == "ord01-000001-00000001"
    assert strategy.checks == {
        "status": m.OrderStatus.SUBMITTED,
        "leaves": "0.010",
        "open": 1,
        "tick": "0.1",
        "cancel": True,
        "cancel_again": False,
        "modify": False,
        "cancel_all": 0,
        "still_open": 1,
    }

    records = list(log.read(result.directory))
    kinds = [r.kind for r in records]
    assert kinds.count("SubmitOrder") == 1
    assert kinds.count("OrderDenied") == 1
    assert kinds.count("CancelOrder") == 1
    denied = next(r.event for r in records if r.kind == "OrderDenied")
    assert str(denied.strategy_id) == "quoter-001"
    assert str(denied.trader_id) == "ORD01-001"

    replay = Node.from_run(result.directory)
    report = replay.add_strategy(Quoter(id="quoter-001")).replay()
    assert report.divergence is None, str(report)
    assert report.outputs == result.outputs


def test_orders_without_a_definition_are_denied(tmp_path: Path) -> None:
    node = Node(_config(tmp_path, define=False), out=tmp_path / "run")
    strategy = Quoter(id="quoter-001")
    node.add_strategy(strategy).run()
    assert strategy.events[:2] == ["OrderDenied INSTRUMENT_UNKNOWN"] * 2
    assert strategy.checks["tick"] is None


def test_order_intents_are_editable_before_submit() -> None:
    intent = jarvis.Context.market(IID, m.OrderSide.SELL, "0.5", reduce_only=True)
    assert intent.order_type == m.OrderType.MARKET
    assert intent.time_in_force == m.TimeInForce.IOC
    assert intent.price is None
    assert intent.reduce_only
    intent.order_type = m.OrderType.LIMIT
    intent.price = "65000.1"
    intent.time_in_force = m.TimeInForce.GTC
    assert str(intent.price) == "65000.1"
