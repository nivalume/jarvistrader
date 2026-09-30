"""Python strategies (docs/architecture.md sections 4.5, 7.5 and 9).

A strategy is a class with callbacks; every callback is optional and the node never calls one a
class does not define. Callbacks receive the ``Context`` first; it, and any ``BookView``,
``TradeBatch`` or ``QuoteBatch`` passed with it, is valid only during that call.

Callbacks::

    on_start(ctx)                          on_stop(ctx)
    on_trade(ctx, trade)                   on_quote(ctx, quote)
    on_book(ctx, book)                     on_book_deltas(ctx, deltas)
    on_bar(ctx, bar)                       on_mark_price(ctx, update)
    on_index_price(ctx, update)            on_funding_rate(ctx, update)
    on_instrument_status(ctx, status)      on_instrument_close(ctx, close)
    on_liquidation(ctx, order)             on_feature(ctx, feature_id, value, ts)
    on_trade_batch(ctx, batch)             on_quote_batch(ctx, batch)
    on_timer(ctx, timer_id, deadline)      on_error(ctx, error)
    on_order_event(ctx, event)             every event of this strategy's orders
    on_position_event(ctx, event)          this strategy's positions: opened, changed, closed,
                                           adjusted (funding)
    on_reconciled(ctx, outcome)            the account was reconciled with the venue (live: at
                                           start, before on_start, and after each reconnect)
    on_params_changed(ctx, key, value)     an operator set a parameter (admin ``set_param``):
                                           value is a bool, an int or the text as given

Orders: ``ctx.submit(ctx.limit(iid, side, qty, price))`` returns the ClientOrderId; the outcome
(submitted or denied, then the venue's answers) arrives in ``on_order_event``. ``ctx.modify``,
``ctx.cancel``, ``ctx.cancel_all``, ``ctx.order`` and ``ctx.open_orders`` complete the set;
``ctx.position``, ``ctx.exposure``, ``ctx.balance`` and ``ctx.trading_state`` read the portfolio
and the risk state. ``ctx.submit_parent(intent, algo="passthrough", params=[])`` hands a parent
order to an execution algorithm (``passthrough``, ``passive_then_aggressive``, ``pegged_quote``;
``params`` are up to four integers the algorithm defines); ``ctx.parent`` reads it and
``ctx.cancel`` cancels it.

An exception in a callback becomes a recorded StrategyError; ``risk.on_strategy_error`` decides
what happens next (by default the strategy stops). Inside callbacks the wall clock and
``random`` are off limits (``jarvis.determinism``); use ``ctx.now()`` and ``ctx.rng(key)``.

EngineState snapshots (docs/architecture.md section 16.3): a strategy that keeps state of its own
defines both ``on_save(self) -> bytes`` and ``on_load(self, state: bytes)``. The node calls
``on_save`` at each snapshot point (``persistence.snapshot_every`` inputs, after a batch) and
``on_load`` with the same bytes when it restores that snapshot; neither gets a context. The
bytes must be a function of the strategy's state alone (a replay compares snapshots byte for
byte), for example ``json.dumps(..., sort_keys=True).encode()``. Orders, positions,
subscriptions and timers are the kernel's and are in the snapshot already. A strategy without
these methods is still snapshotted, but restoring leaves it as constructed, so a node recovers
it by replaying the log instead.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from typing import Any, ClassVar

from ._core import node as _native

BookView = _native.BookView
Cadence = _native.Cadence
Context = _native.Context
DataKind = _native.DataKind
OrderIntent = _native.OrderIntent
OrderView = _native.OrderView
ParentView = _native.ParentView
PositionView = _native.PositionView
ExposureView = _native.ExposureView
QuoteBatch = _native.QuoteBatch
TradeBatch = _native.TradeBatch

CALLBACKS = (
    "on_start",
    "on_stop",
    "on_trade",
    "on_quote",
    "on_book",
    "on_book_deltas",
    "on_bar",
    "on_mark_price",
    "on_index_price",
    "on_funding_rate",
    "on_instrument_status",
    "on_instrument_close",
    "on_liquidation",
    "on_feature",
    "on_trade_batch",
    "on_quote_batch",
    "on_timer",
    "on_error",
    "on_order_event",
    "on_position_event",
    "on_reconciled",
    "on_params_changed",
)


class Strategy:
    """Base class for Python strategies.

    ``params`` holds class defaults; the ``[[strategies]]`` entry's ``params`` override them.
    Subclasses that define ``__init__`` must accept and forward the same arguments.
    """

    params: ClassVar[Mapping[str, Any]] = {}

    def __init__(
        self,
        params: Mapping[str, Any] | None = None,
        *,
        id: str | None = None,  # noqa: A002 - the configuration's name for it
        instruments: Iterable[Any] = (),
    ) -> None:
        merged = dict(type(self).params)
        if params:
            merged.update(params)
        self.params: dict[str, Any] = merged
        self.id: str = id or f"{type(self).__name__.lower()}-001"
        self.instruments: list[Any] = list(instruments)

    def __repr__(self) -> str:
        return f"{type(self).__name__}(id={self.id!r}, params={self.params!r})"


__all__ = [
    "CALLBACKS",
    "BookView",
    "Cadence",
    "Context",
    "DataKind",
    "ExposureView",
    "OrderIntent",
    "OrderView",
    "ParentView",
    "PositionView",
    "QuoteBatch",
    "Strategy",
    "TradeBatch",
]
