"""Helpers shared by the converters: decimal text to raw values, precisions, catalog paths."""

from __future__ import annotations

import datetime as _dt
import os
import shutil
from collections.abc import Iterable
from pathlib import Path

from .. import log
from .. import model as m

SCALE = 9  # raw values are integers at 10^9 (nautilus standard precision)
_POW10 = [10**i for i in range(19)]


def raw_of(text: str) -> int:
    """'81225.7' -> 81225700000000000. Exact; more than nine decimals is an error."""
    negative = text.startswith("-")
    if negative or text.startswith("+"):
        text = text[1:]
    whole, _, frac = text.partition(".")
    frac = frac.rstrip("0")
    if len(frac) > SCALE:
        raise ValueError(f"{text!r} has more than {SCALE} decimals")
    raw = int(whole or "0") * _POW10[SCALE] + (int(frac) * _POW10[SCALE - len(frac)] if frac else 0)
    return -raw if negative else raw


def decimals_of(text: str) -> int:
    """Decimals actually used by a decimal text ('0.33090000' -> 4)."""
    _, _, frac = text.partition(".")
    return len(frac.rstrip("0"))


def ns_of(timestamp: str) -> int:
    """A Binance epoch timestamp in ms (or us, used by spot files since 2025) as nanoseconds."""
    value = int(timestamp)
    return value * 1_000 if value >= 10**14 else value * 1_000_000


def utc_day(ns: int) -> str:
    return _dt.datetime.fromtimestamp(ns // 1_000_000_000, tz=_dt.timezone.utc).strftime("%Y-%m-%d")


def catalog_dir(catalog: str | os.PathLike[str], instrument_id: str, stream: str, day: str) -> Path:
    """{catalog}/{instrument_id}/{stream}/{YYYY-MM-DD}: the layout the backtest node reads."""
    return Path(catalog) / instrument_id / stream / day


class MonotonicClock:
    """Keeps key timestamps non-decreasing within one source: a record that goes back in time is
    keyed at the previous timestamp (its ts_event keeps the exchange's value)."""

    def __init__(self) -> None:
        self.last = 0
        self.clamped = 0

    def __call__(self, ns: int) -> int:
        if ns < self.last:
            self.clamped += 1
            return self.last
        self.last = ns
        return ns


def write_log(
    directory: Path, events: Iterable[tuple[int, object]], *, source_id: int, overwrite: bool
) -> int:
    """Writes (ts, event) pairs as one log with seq = 1, 2, ...; returns the record count."""
    if directory.exists():
        if not overwrite:
            raise FileExistsError(f"{directory} exists; pass overwrite=True to replace it")
        shutil.rmtree(directory)
    count = 0
    with log.EventLogWriter(str(directory)) as writer:
        for count, (ts, event) in enumerate(events, start=1):
            writer.append(event, seq=count, ts=ts, source_id=source_id)
    return count


def price(raw: int, precision: int) -> m.Price:
    return m.Price.from_raw(raw, precision)


def quantity(raw: int, precision: int) -> m.Quantity:
    return m.Quantity.from_raw(raw, precision)
