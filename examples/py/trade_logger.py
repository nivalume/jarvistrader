"""Trade logger: records market activity and never trades (docs/plan.md M2).

Subscribes to every trade and to quotes sampled once per ``sample_ms``, and records

    mid      the mid price of each sampled quote
    trades   the number of trades in each ``window_s`` window (on a timer)
    volume   the traded quantity in that window

The values are computed on raw integers so that examples/cpp/trade_logger.cpp, which does the
same in C++, writes a byte-identical run log for the same data.

    python examples/py/trade_logger.py --config examples/config/trade_logger.toml --env backtest
    python examples/py/trade_logger.py --replay runs/trade_logger/<run>
"""

from __future__ import annotations

from typing import Any

import jarvis
from jarvis import Cadence

SCALE = 1_000_000_000


def fixed9(raw: int) -> str:
    """A raw value at 10^9 as decimal text with nine decimals (the precision C++ uses)."""
    sign = "-" if raw < 0 else ""
    raw = abs(raw)
    return f"{sign}{raw // SCALE}.{raw % SCALE:09d}"


class TradeLogger(jarvis.Strategy):
    params = {"instrument": "BTCUSDT-PERP.BINANCE", "sample_ms": 1000, "window_s": 60}

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.trades = 0
        self.volume_raw = 0

    def on_start(self, ctx: jarvis.Context) -> None:
        instrument = self.params["instrument"]
        ctx.subscribe_trades(instrument)
        ctx.subscribe_quotes(instrument, Cadence.sampled_ms(self.params["sample_ms"]))
        window = self.params["window_s"] * SCALE
        ctx.set_timer(1, (ctx.now() // window + 1) * window, window)  # aligned windows

    def on_trade(self, ctx: jarvis.Context, trade: Any) -> None:
        self.trades += 1
        self.volume_raw += trade.size.raw

    def on_quote(self, ctx: jarvis.Context, quote: Any) -> None:
        ctx.record("mid", fixed9((quote.bid_price.raw + quote.ask_price.raw) // 2))

    def on_timer(self, ctx: jarvis.Context, timer_id: int, deadline: int) -> None:
        ctx.record("trades", self.trades)
        ctx.record("volume", fixed9(self.volume_raw))
        self.trades = 0
        self.volume_raw = 0


if __name__ == "__main__":
    jarvis.main(TradeLogger)
