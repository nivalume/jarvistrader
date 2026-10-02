"""Trend follower: an EMA crossover on sampled mids, traded with market orders.

Every ``sample_s`` seconds it updates a fast and a slow EMA of the mid price. Once ``slow``
samples have passed it wants to be long ``size`` when the fast EMA is more than ``band_bps``
above the slow one, short when it is that far below, and keeps its last side in between. It
moves to the wanted position with one market order (a flip is one order of twice the size) and
waits for that order to finish before it sends the next. Every fill takes liquidity, so fees and
latency show in the metrics. For tests and demos only.

After the backtest the script prints the run's metrics (examples/py/backtest_metrics.py):

    python examples/py/trend_follow.py --config examples/config/trend_follow.toml --out runs/trend
    python examples/py/backtest_metrics.py runs/trend       # the metrics of a recorded run again
"""

from __future__ import annotations

import argparse
import sys
from decimal import Decimal
from typing import Any

import backtest_metrics
import jarvis
from jarvis import Cadence, determinism
from jarvis import model as m

OPEN = {
    m.OrderStatus.SUBMITTED,
    m.OrderStatus.ACCEPTED,
    m.OrderStatus.TRIGGERED,
    m.OrderStatus.PENDING_UPDATE,
    m.OrderStatus.PENDING_CANCEL,
    m.OrderStatus.PARTIALLY_FILLED,
}


class TrendFollow(jarvis.Strategy):
    params = {
        "instrument": "BTCUSDT-PERP.BINANCE",
        "size": "0.010",
        "sample_s": 10,
        "fast": 30,
        "slow": 120,
        "band_bps": 2,
    }

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.size = Decimal(self.params["size"])
        self.alpha_fast = 2 / (self.params["fast"] + 1)
        self.alpha_slow = 2 / (self.params["slow"] + 1)
        self.band = self.params["band_bps"] / 10_000
        self.fast: float | None = None
        self.slow: float | None = None
        self.samples = 0
        self.side = 0  # the side wanted: 1 long, -1 short, 0 not decided yet
        self.order: Any = None

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(
            self.params["instrument"], Cadence.sampled_ms(self.params["sample_s"] * 1000)
        )

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        mid = (quote.bid_price.raw + quote.ask_price.raw) / 2e9
        if self.fast is None or self.slow is None:
            self.fast = self.slow = mid
        else:
            self.fast += self.alpha_fast * (mid - self.fast)
            self.slow += self.alpha_slow * (mid - self.slow)
        self.samples += 1
        if self.samples < self.params["slow"]:
            return
        if self.fast > self.slow * (1 + self.band):
            self.side = 1
        elif self.fast < self.slow * (1 - self.band):
            self.side = -1
        if self.side != 0:
            self._move_to(ctx, self.size * self.side)

    def _move_to(self, ctx: jarvis.Context, wanted: Decimal) -> None:
        view = ctx.order(self.order) if self.order is not None else None
        if view is not None and view.status in OPEN:
            return
        iid = self.params["instrument"]
        position = ctx.position(iid)
        delta = wanted - (position.signed_qty if position is not None else Decimal(0))
        if delta == 0:
            return
        side = m.OrderSide.BUY if delta > 0 else m.OrderSide.SELL
        quantity = m.Quantity(str(abs(delta).quantize(self.size)))
        self.order = ctx.submit(ctx.market(iid, side, quantity))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Backtest TrendFollow and print its metrics.")
    parser.add_argument("--config", required=True)
    parser.add_argument("--env")
    parser.add_argument("--set", dest="sets", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--out", help="the run directory")
    parser.add_argument("--run-for", type=float, help="sandbox and live: seconds to run")
    parser.add_argument("--interval", type=int, default=60, help="equity sampling interval, s")
    parser.add_argument("--json", metavar="FILE", help="also write the metrics as JSON")
    args = parser.parse_args(argv)
    try:
        node = jarvis.Node(args.config, env=args.env, sets=args.sets, out=args.out)
        if argv is None:
            determinism.ensure_hash_seed(node.seed)  # as jarvis.main does
        for strategy in jarvis.node.build_strategies(node.strategy_entries, [TrendFollow]):
            node.add_strategy(strategy)
        result = node.run(run_for=args.run_for)
    except (ValueError, OSError) as e:
        print(f"trend_follow: {e}", file=sys.stderr)
        return 1
    print(result)
    if not result.directory:
        print("the run was not recorded (persistence.mode = none): no metrics", file=sys.stderr)
        return 1
    print()
    extra = ["--json", args.json] if args.json else []
    code = backtest_metrics.main([result.directory, "--interval", str(args.interval), *extra])
    return code or (1 if result.halted else 0)


if __name__ == "__main__":
    sys.exit(main())
