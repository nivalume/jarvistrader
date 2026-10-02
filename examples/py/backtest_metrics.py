"""Metrics of a recorded run: PnL, activity, round trips, equity curve, drawdown and Sharpe.

Works on any run directory (a backtest of any strategy, or a sandbox run), from the directory
alone. Two sources, each used for what it is good at:

    RunReport   fills, fees, PnL and order counts, computed by the kernel's own OMS and portfolio
    run log     the equity curve, rebuilt from the OrderFilled and TradeTick inputs, from which
                the drawdown, the Sharpe ratio and the round trips follow

The equity curve marks positions at the last trade price, the valuation RunReport uses when
there is no mark price stream, so the curve ends where the report's net PnL does; ``check``
shows the difference (it should be 0, funding aside, which the curve does not include).
Instruments must be linear (USDT-margined) and the account single-currency.

    python examples/py/backtest_metrics.py runs/trend [--interval 60] [--json metrics.json]
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import statistics
import sys
from decimal import Decimal

import jarvis
from jarvis import log
from jarvis import model as m

YEAR_S = 365 * 24 * 3600  # crypto trades around the clock


@dataclasses.dataclass(frozen=True)
class Metrics:
    directory: str
    currency: str
    duration_s: float
    # Account (RunReport).
    start_equity: Decimal
    end_equity: Decimal
    net_pnl: Decimal
    return_pct: float
    realized_pnl: Decimal
    unrealized_pnl: Decimal
    commission: Decimal
    funding: Decimal
    # Activity (RunReport).
    fills: int
    maker_fills: int
    taker_fills: int
    notional: Decimal
    turnover: float  # traded notional over starting equity
    orders_submitted: int
    orders_filled: int
    orders_rejected: int  # denied by risk or rejected by the venue
    # Round trips (run log): a position from flat to flat, or until it flips.
    round_trips: int
    win_rate: float | None
    profit_factor: float | None  # gross profit over gross loss, after fees
    avg_win: Decimal | None
    avg_loss: Decimal | None
    expectancy: Decimal | None  # mean net PnL of a round trip
    # Risk (run log).
    max_abs_position: Decimal
    max_drawdown: Decimal
    max_drawdown_pct: float
    sharpe: float | None  # annualized from `interval_s` equity returns, risk-free rate 0
    interval_s: int
    samples: int
    check: Decimal  # equity curve's net PnL minus the report's (funding excluded)


class _Trades:
    """Round trips of one instrument with the average-cost method; fees count in the trip."""

    def __init__(self) -> None:
        self.position = Decimal(0)
        self.avg = Decimal(0)
        self.gross = Decimal(0)
        self.fees = Decimal(0)
        self.done: list[Decimal] = []  # net PnL of each finished round trip

    def fill(self, signed_qty: Decimal, price: Decimal, fee: Decimal) -> None:
        qty = abs(signed_qty)
        if self.position == 0 or (self.position > 0) == (signed_qty > 0):
            self.avg = (self.avg * abs(self.position) + price * qty) / (abs(self.position) + qty)
            self.position += signed_qty
            self.fees += fee
            return
        closed = min(qty, abs(self.position))
        direction = 1 if self.position > 0 else -1
        self.gross += (price - self.avg) * closed * direction
        self.fees += fee * closed / qty
        self.position += signed_qty
        if self.position == 0 or abs(signed_qty) > closed:  # flat, or flipped
            self.done.append(self.gross - self.fees)
            self.gross = Decimal(0)
            self.fees = fee * (qty - closed) / qty
            self.avg = price
            if self.position == 0:
                self.avg = Decimal(0)


def _curve(
    directory: str, start: Decimal, interval_s: int
) -> tuple[list[Decimal], Decimal, dict[str, _Trades]]:
    """Equity at the first trade or fill of each interval, and at the end of the run; the
    largest position held; the round trips."""
    interval_ns = interval_s * 1_000_000_000
    cash = Decimal(0)
    position: dict[str, Decimal] = {}
    mark: dict[str, Decimal] = {}
    trades: dict[str, _Trades] = {}
    max_position = Decimal(0)
    points: list[Decimal] = []
    bucket = None

    def equity() -> Decimal:
        return start + cash + sum((q * mark[i] for i, q in position.items()), Decimal(0))

    for record in log.read(directory):
        if record.is_output or record.kind not in ("TradeTick", "OrderFilled"):
            continue
        event = record.event
        iid = str(event.instrument_id)
        if record.kind == "TradeTick":
            mark[iid] = event.price.as_decimal()
        else:
            qty = event.last_qty.as_decimal()
            signed = qty if event.order_side == m.OrderSide.BUY else -qty
            price = event.last_px.as_decimal()
            fee = event.commission.as_decimal()
            cash -= signed * price + fee
            position[iid] = position.get(iid, Decimal(0)) + signed
            mark.setdefault(iid, price)
            max_position = max(max_position, abs(position[iid]))
            trades.setdefault(iid, _Trades()).fill(signed, price, fee)
        if record.ts // interval_ns != bucket:
            bucket = record.ts // interval_ns
            points.append(equity())
    points.append(equity())
    return points, max_position, trades


def _max_drawdown(points: list[Decimal]) -> tuple[Decimal, float]:
    peak = points[0]
    worst = Decimal(0)
    worst_pct = 0.0
    for p in points:
        peak = max(peak, p)
        worst = max(worst, peak - p)
        worst_pct = max(worst_pct, float((peak - p) / peak))
    return worst, worst_pct


def _sharpe(points: list[Decimal], interval_s: int) -> float | None:
    returns = [float(b / a - 1) for a, b in zip(points, points[1:], strict=False)]
    if len(returns) < 3:
        return None
    spread = statistics.stdev(returns)
    if spread == 0:
        return None
    return statistics.fmean(returns) / spread * math.sqrt(YEAR_S / interval_s)


def compute(directory: str, interval_s: int = 60) -> Metrics:
    report = jarvis.RunReport.from_run(directory)
    currencies = {str(b.currency) for b in report.starting_balances}
    if len(currencies) != 1:
        raise ValueError(f"single-currency accounts only, the run has {sorted(currencies)}")
    start = sum((b.as_decimal() for b in report.starting_balances), Decimal(0))
    rows = report.rows
    net = sum((r.net_pnl.as_decimal() for r in rows), Decimal(0))
    funding = sum((r.funding.as_decimal() for r in rows), Decimal(0))
    notional = sum((r.notional.as_decimal() for r in rows), Decimal(0))
    orders = list(report.orders.values())

    points, max_position, trades = _curve(directory, start, interval_s)
    finished = [pnl for t in trades.values() for pnl in t.done]
    wins = [p for p in finished if p > 0]
    losses = [p for p in finished if p < 0]
    drawdown, drawdown_pct = _max_drawdown(points)
    return Metrics(
        directory=directory,
        currency=next(iter(currencies)),
        duration_s=(report.last_ts - report.first_ts) / 1e9,
        start_equity=start,
        end_equity=start + net,
        net_pnl=net,
        return_pct=float(net / start * 100),
        realized_pnl=sum((r.realized_pnl.as_decimal() for r in rows), Decimal(0)),
        unrealized_pnl=sum((r.unrealized_pnl.as_decimal() for r in rows), Decimal(0)),
        commission=sum((r.commission.as_decimal() for r in rows), Decimal(0)),
        funding=funding,
        fills=sum(r.fills for r in rows),
        maker_fills=sum(r.maker_fills for r in rows),
        taker_fills=sum(r.taker_fills for r in rows),
        notional=notional,
        turnover=float(notional / start),
        orders_submitted=sum(o.submitted for o in orders),
        orders_filled=sum(o.filled for o in orders),
        orders_rejected=sum(o.denied + o.rejected for o in orders),
        round_trips=len(finished),
        win_rate=len(wins) / len(finished) if finished else None,
        profit_factor=float(sum(wins) / -sum(losses)) if wins and losses else None,
        avg_win=sum(wins) / len(wins) if wins else None,
        avg_loss=sum(losses) / len(losses) if losses else None,
        expectancy=sum(finished) / len(finished) if finished else None,
        max_abs_position=max_position,
        max_drawdown=drawdown,
        max_drawdown_pct=drawdown_pct * 100,
        sharpe=_sharpe(points, interval_s),
        interval_s=interval_s,
        samples=len(points),
        check=(points[-1] - start) - (net - funding),
    )


def render(x: Metrics) -> str:
    def money(v: Decimal | None) -> str:
        return "n/a" if v is None else f"{v:,.2f} {x.currency}"

    def ratio(v: float | None, spec: str = ".2f") -> str:
        return "n/a" if v is None else format(v, spec)

    lines = [
        f"run: {x.directory}  ({x.duration_s / 3600:.2f} h of data)",
        "",
        "account",
        f"  start equity     {money(x.start_equity)}",
        f"  end equity       {money(x.end_equity)}",
        f"  net PnL          {money(x.net_pnl)}  ({x.return_pct:+.3f}%)",
        f"    realized       {money(x.realized_pnl)}",
        f"    unrealized     {money(x.unrealized_pnl)}",
        f"    commission     {money(-x.commission)}",
        f"    funding        {money(x.funding)}",
        "activity",
        f"  fills            {x.fills}  (maker {x.maker_fills}, taker {x.taker_fills})",
        f"  traded notional  {money(x.notional)}  (turnover {x.turnover:.1f}x)",
        f"  orders           {x.orders_submitted} submitted, {x.orders_filled} filled, "
        f"{x.orders_rejected} denied or rejected",
        "round trips",
        f"  count            {x.round_trips}",
        f"  win rate         {ratio(None if x.win_rate is None else x.win_rate * 100, '.1f')}%",
        f"  profit factor    {ratio(x.profit_factor)}",
        f"  avg win / loss   {money(x.avg_win)} / {money(x.avg_loss)}",
        f"  expectancy       {money(x.expectancy)} per round trip",
        "risk",
        f"  max position     {x.max_abs_position}",
        f"  max drawdown     {money(x.max_drawdown)}  ({x.max_drawdown_pct:.3f}%)",
        f"  sharpe           {ratio(x.sharpe)}  (annualized, {x.samples} equity points "
        f"every {x.interval_s} s, risk-free 0)",
        "",
        f"check: equity curve minus report net PnL = {x.check:.8f} {x.currency}",
    ]
    return "\n".join(lines)


def _json_default(value: object) -> str:
    return str(value)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Metrics of a recorded run.")
    parser.add_argument("directory", help="the run directory")
    parser.add_argument("--interval", type=int, default=60, help="equity sampling interval, s")
    parser.add_argument("--json", metavar="FILE", help="also write the metrics as JSON")
    args = parser.parse_args(argv)
    try:
        metrics = compute(args.directory, args.interval)
    except (ValueError, OSError) as e:
        print(f"backtest_metrics: {e}", file=sys.stderr)
        return 1
    print(render(metrics))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(dataclasses.asdict(metrics), f, indent=2, default=_json_default)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
