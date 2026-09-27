"""Backtest reports (docs/architecture.md section 12.4).

``RunReport.from_run(directory)`` (or ``result.report()``) reads a run directory and reports, per
strategy and instrument, the fills, fees and PnL, the orders' fate and the account, labelled
with the data and the simulated venue's models the numbers rest on. The kernel's own OMS and
Portfolio code recompute them from the run log, so they match what the strategies saw.
"""

from __future__ import annotations

import dataclasses
import os
from collections.abc import Mapping
from typing import Any

from ._core import node as _native


@dataclasses.dataclass(frozen=True)
class ReportRow:
    strategy: str
    instrument: str
    fills: int
    maker_fills: int
    taker_fills: int
    bought: Any  # Quantity
    sold: Any  # Quantity
    notional: Any  # Money, settlement currency
    commission: Any  # Money paid
    funding: Any  # Money received (negative when paid)
    realized_pnl: Any  # Money, gross of commission and funding
    unrealized_pnl: Any  # Money at the valuation price
    net_pnl: Any  # realized - commission + funding + unrealized
    position: Any  # Decimal, signed
    avg_px_open: Any  # Price or None
    valuation: Any  # Price or None
    valuation_source: str  # "mark", "last trade", "last quote mid" or "none"


@dataclasses.dataclass(frozen=True)
class OrderCounts:
    submitted: int
    denied: int
    accepted: int
    rejected: int
    canceled: int
    expired: int
    filled: int
    modifies: int
    cancels: int
    modify_rejected: int
    cancel_rejected: int
    refused: int  # venue events the OMS refused (late or duplicate)


@dataclasses.dataclass(frozen=True)
class RunReport:
    directory: str
    node_id: str
    env: str
    seed: int
    catalog: str
    range: str
    streams: tuple[str, ...]
    instruments: tuple[str, ...]
    book: str  # the market data fills were simulated against, e.g. "L1 (bookTicker) + trades"
    venue: str  # the simulated venue's models, or "none (data only)"
    fill_model: str
    fee_schedule: str
    starting_balances: tuple[Any, ...]
    ending_balances: tuple[Any, ...]
    rows: tuple[ReportRow, ...]
    orders: Mapping[str, OrderCounts]
    first_ts: int
    last_ts: int
    text: str

    @classmethod
    def from_run(cls, directory: str | os.PathLike[str]) -> RunReport:
        raw = dict(_native.run_report(os.fspath(directory)))
        raw["streams"] = tuple(raw["streams"])
        raw["instruments"] = tuple(raw["instruments"])
        raw["starting_balances"] = tuple(raw["starting_balances"])
        raw["ending_balances"] = tuple(raw["ending_balances"])
        raw["rows"] = tuple(ReportRow(**row) for row in raw["rows"])
        raw["orders"] = {name: OrderCounts(**c) for name, c in raw["orders"].items()}
        return cls(**raw)

    def row(self, strategy: str, instrument: str) -> ReportRow | None:
        for r in self.rows:
            if r.strategy == strategy and r.instrument == instrument:
                return r
        return None

    def __str__(self) -> str:
        return self.text


__all__ = ["OrderCounts", "ReportRow", "RunReport"]
