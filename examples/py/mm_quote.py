"""Market maker: quotes both sides at the touch (docs/plan.md M3). For tests and soak runs only.

On each sampled quote it keeps one post-only order per side at the best bid and the best ask,
moved with modifies as the touch moves. Inventory skews the side that would grow it by
``skew_ticks`` and switches that side off at ``max_position``. It never crosses the spread, so
every fill is a maker fill of the simulated venue's queue model.

examples/cpp/pegged_mm.cpp makes the same decisions in C++; for the same data both write
byte-identical run logs (tools/m3_acceptance.sh checks it).

    python examples/py/mm_quote.py --config examples/config/mm_quote.toml --env backtest
    python examples/py/mm_quote.py --replay runs/mm01/<run>
"""

from __future__ import annotations

from typing import Any

import jarvis
from jarvis import Cadence
from jarvis import model as m

OPEN = {
    m.OrderStatus.SUBMITTED,
    m.OrderStatus.ACCEPTED,
    m.OrderStatus.TRIGGERED,
    m.OrderStatus.PENDING_UPDATE,
    m.OrderStatus.PENDING_CANCEL,
    m.OrderStatus.PARTIALLY_FILLED,
}
PENDING = {m.OrderStatus.PENDING_UPDATE, m.OrderStatus.PENDING_CANCEL}


class MmQuote(jarvis.Strategy):
    params = {
        "instrument": "BTCUSDT-PERP.BINANCE",
        "size": "0.010",
        "max_position": "0.050",
        "skew_ticks": 1,
        "sample_ms": 1000,
    }

    def __init__(self, params: Any = None, **kwargs: Any) -> None:
        super().__init__(params, **kwargs)
        self.bid: Any = None
        self.ask: Any = None
        self.size = m.Quantity(self.params["size"])
        self.max_raw = m.Quantity(self.params["max_position"]).raw

    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(self.params["instrument"], Cadence.sampled_ms(self.params["sample_ms"]))

    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        iid = self.params["instrument"]
        definition = ctx.instrument(iid)
        if definition is None:
            return
        tick = definition.price_increment.raw
        precision = definition.price_precision
        position = ctx.position(iid)
        pos = int(position.signed_qty.scaleb(9)) if position is not None else 0  # raw, 10^9
        skew = self.params["skew_ticks"] * tick
        bid = quote.bid_price.raw - (skew if pos > 0 else 0)
        ask = quote.ask_price.raw + (skew if pos < 0 else 0)
        size = self.size.raw
        self.bid = self._side(ctx, self.bid, m.OrderSide.BUY, bid, precision,
                              pos + size <= self.max_raw)
        self.ask = self._side(ctx, self.ask, m.OrderSide.SELL, ask, precision,
                              pos - size >= -self.max_raw)

    def _side(self, ctx: jarvis.Context, order_id: Any, side: Any, price_raw: int, precision: int,
              active: bool) -> Any:
        view = ctx.order(order_id) if order_id is not None else None
        working = view is not None and view.status in OPEN
        if not active:
            if working and view.status != m.OrderStatus.PENDING_CANCEL:
                ctx.cancel(order_id)
            return order_id if working else None
        price = m.Price.from_raw(price_raw, precision)
        if not working:
            intent = ctx.limit(self.params["instrument"], side, self.size, price, post_only=True)
            return ctx.submit(intent)
        if view.status not in PENDING and view.price.raw != price_raw:
            ctx.modify(order_id, price=price)
        return order_id


if __name__ == "__main__":
    jarvis.main(MmQuote)
