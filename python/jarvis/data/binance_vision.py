"""data.binance.vision archives to catalog logs (docs/architecture.md section 16.1).

Datasets (USDⓈ-M futures ``market="um"`` and spot ``market="spot"``):

    aggTrades         -> TradeTick           stream "aggTrade"                 source 1
    bookTicker        -> QuoteTick           stream "bookTicker"               source 2
    klines            -> Bar (EXTERNAL)      stream "kline_<interval>"         source 3
    markPriceKlines   -> MarkPriceUpdate     stream "markPriceKline_<interval>" source 4
    indexPriceKlines  -> IndexPriceUpdate    stream "indexPriceKline_<interval>" source 5

Each file becomes ``{catalog}/{instrument_id}/{stream}/{YYYY-MM-DD}/``. Timestamps: a trade's
ts_event is its transact_time; a quote's ts_event is the transaction time and its ts_init the
event time (when Binance sent it); a kline-derived event is stamped at the kline's close
(open_time + interval), as bars are in nautilus backtests. ts_init is kept non-decreasing within
a file. Precisions default to the most decimals any value in the file uses (bookTicker prints
eight decimals whatever the tick size, so trailing zeros are ignored); pass them explicitly to
pin them to the instrument's tick and step sizes.

Futures bookTicker files are published up to spring 2024 only; later days have no quote archive.
"""

from __future__ import annotations

import csv
import hashlib
import io
import os
import urllib.request
import zipfile
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from .. import model as m
from ._common import (
    MonotonicClock,
    catalog_dir,
    decimals_of,
    ns_of,
    price,
    quantity,
    raw_of,
    utc_day,
    write_log,
)

BASE_URL = "https://data.binance.vision/data"

SOURCE_IDS = {
    "aggTrades": 1,
    "bookTicker": 2,
    "klines": 3,
    "markPriceKlines": 4,
    "indexPriceKlines": 5,
}

_INTERVALS = {
    "1s": (1, "SECOND", 1_000_000_000),
    "1m": (1, "MINUTE", 60_000_000_000),
    "3m": (3, "MINUTE", 180_000_000_000),
    "5m": (5, "MINUTE", 300_000_000_000),
    "15m": (15, "MINUTE", 900_000_000_000),
    "30m": (30, "MINUTE", 1_800_000_000_000),
    "1h": (1, "HOUR", 3_600_000_000_000),
    "2h": (2, "HOUR", 7_200_000_000_000),
    "4h": (4, "HOUR", 14_400_000_000_000),
    "6h": (6, "HOUR", 21_600_000_000_000),
    "8h": (8, "HOUR", 28_800_000_000_000),
    "12h": (12, "HOUR", 43_200_000_000_000),
    "1d": (1, "DAY", 86_400_000_000_000),
}


@dataclass(frozen=True)
class Converted:
    directory: Path
    instrument_id: str
    stream: str
    records: int
    price_precision: int
    size_precision: int
    clamped: int  # records whose ts_init was raised to keep the source in order


def instrument_id_for(symbol: str, market: str = "um") -> str:
    """BTCUSDT (um) -> BTCUSDT-PERP.BINANCE; a dated contract or a spot symbol keeps its name."""
    if market == "um" and "_" not in symbol:
        return f"{symbol}-PERP.BINANCE"
    return f"{symbol}.BINANCE"


def stream_for(dataset: str, interval: str = "1m") -> str:
    names = {
        "aggTrades": "aggTrade",
        "bookTicker": "bookTicker",
        "klines": f"kline_{interval}",
        "markPriceKlines": f"markPriceKline_{interval}",
        "indexPriceKlines": f"indexPriceKline_{interval}",
    }
    if dataset not in names:
        raise ValueError(f"unknown dataset {dataset!r}; expected one of {', '.join(names)}")
    return names[dataset]


def archive_url(dataset: str, symbol: str, date: str, *, market: str = "um", interval: str = "1m") -> str:
    """URL of one daily archive on data.binance.vision."""
    prefix = f"{BASE_URL}/futures/um/daily" if market == "um" else f"{BASE_URL}/spot/daily"
    if dataset in ("klines", "markPriceKlines", "indexPriceKlines"):
        return f"{prefix}/{dataset}/{symbol}/{interval}/{symbol}-{interval}-{date}.zip"
    return f"{prefix}/{dataset}/{symbol}/{symbol}-{dataset}-{date}.zip"


def download(
    dataset: str,
    symbol: str,
    date: str,
    dest: str | os.PathLike[str],
    *,
    market: str = "um",
    interval: str = "1m",
) -> Path:
    """Downloads one daily archive and checks it against the published SHA-256 CHECKSUM file."""
    url = archive_url(dataset, symbol, date, market=market, interval=interval)
    target = Path(dest) / url.rsplit("/", 1)[1]
    target.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url + ".CHECKSUM", timeout=60) as r:  # noqa: S310 - fixed host
        expected = r.read().decode().split()[0]
    if not target.exists() or _sha256(target) != expected:
        with urllib.request.urlopen(url, timeout=600) as r, open(target, "wb") as f:  # noqa: S310
            while chunk := r.read(1 << 20):
                f.write(chunk)
    actual = _sha256(target)
    if actual != expected:
        target.unlink()
        raise ValueError(f"{url}: checksum mismatch ({actual} != {expected})")
    return target


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def _rows(source: Path) -> Iterator[list[str]]:
    """CSV rows of a .zip archive or a .csv file, without the header row newer files carry."""
    if source.suffix == ".zip":
        with zipfile.ZipFile(source) as z:
            [name] = [n for n in z.namelist() if n.endswith(".csv")]
            with z.open(name) as raw:
                yield from _csv_rows(io.TextIOWrapper(raw, encoding="ascii", newline=""))
    else:
        with open(source, newline="", encoding="ascii") as f:
            yield from _csv_rows(f)


def _csv_rows(stream: io.TextIOBase) -> Iterator[list[str]]:
    reader = csv.reader(stream)
    for row in reader:
        if row and row[0] and (row[0][0].isdigit()):
            yield row


def _precisions(source: Path, price_cols: tuple[int, ...], size_cols: tuple[int, ...]) -> tuple[int, int]:
    p = s = 0
    for row in _rows(source):
        for c in price_cols:
            p = max(p, decimals_of(row[c]))
        for c in size_cols:
            s = max(s, decimals_of(row[c]))
    return p, s


def convert(
    dataset: str,
    source: str | os.PathLike[str],
    catalog: str | os.PathLike[str],
    *,
    symbol: str,
    market: str = "um",
    interval: str = "1m",
    price_precision: int | None = None,
    size_precision: int | None = None,
    overwrite: bool = False,
) -> Converted:
    """Converts one daily archive (.zip or .csv) into its catalog log."""
    source = Path(source)
    iid_text = instrument_id_for(symbol, market)
    iid = m.InstrumentId.from_str(iid_text)
    stream = stream_for(dataset, interval)
    columns = {
        "aggTrades": ((1,), (2,)),
        "bookTicker": ((1, 3), (2, 4)),
        "klines": ((1, 2, 3, 4), (5,)),
        "markPriceKlines": ((1, 2, 3, 4), ()),
        "indexPriceKlines": ((1, 2, 3, 4), ()),
    }[dataset]
    if price_precision is None or size_precision is None:
        p, s = _precisions(source, *columns)
        price_precision = p if price_precision is None else price_precision
        size_precision = s if size_precision is None else size_precision
    first = next(_rows(source), None)
    if first is None:
        raise ValueError(f"{source} has no rows")
    first_ts = ns_of(first[5] if dataset in ("aggTrades", "bookTicker") else first[0])
    directory = catalog_dir(catalog, iid_text, stream, utc_day(first_ts))
    clock = MonotonicClock()
    events = _events(dataset, source, iid, iid_text, interval, price_precision, size_precision, clock)
    records = write_log(directory, events, source_id=SOURCE_IDS[dataset], overwrite=overwrite)
    return Converted(directory, iid_text, stream, records, price_precision, size_precision, clock.clamped)


def _events(
    dataset: str,
    source: Path,
    iid: m.InstrumentId,
    iid_text: str,
    interval: str,
    pp: int,
    sp: int,
    clock: MonotonicClock,
) -> Iterator[tuple[int, object]]:
    rows = _rows(source)
    if dataset == "aggTrades":
        buy, sell = m.AggressorSide.BUY, m.AggressorSide.SELL
        for r in rows:
            ts_event = ns_of(r[5])
            ts = clock(ts_event)
            side = sell if r[6] in ("true", "True") else buy  # the buyer was the maker
            yield ts, m.TradeTick(
                iid, price(raw_of(r[1]), pp), quantity(raw_of(r[2]), sp), side, m.TradeId(r[0]),
                ts_event, ts,
            )
    elif dataset == "bookTicker":
        for r in rows:
            ts_event = ns_of(r[5])
            ts = clock(max(ns_of(r[6]), ts_event))
            yield ts, m.QuoteTick(
                iid, price(raw_of(r[1]), pp), price(raw_of(r[3]), pp),
                quantity(raw_of(r[2]), sp), quantity(raw_of(r[4]), sp), ts_event, ts,
            )
    else:
        step, unit, length = _INTERVALS[interval]
        bar_type = m.BarType.from_str(f"{iid_text}-{step}-{unit}-LAST-EXTERNAL")
        for r in rows:
            close_ns = ns_of(r[0]) + length
            ts = clock(close_ns)
            if dataset == "klines":
                yield ts, m.Bar(
                    bar_type, price(raw_of(r[1]), pp), price(raw_of(r[2]), pp),
                    price(raw_of(r[3]), pp), price(raw_of(r[4]), pp), quantity(raw_of(r[5]), sp),
                    close_ns, ts,
                )
            elif dataset == "markPriceKlines":
                yield ts, m.MarkPriceUpdate(iid, price(raw_of(r[4]), pp), close_ns, ts)
            else:
                yield ts, m.IndexPriceUpdate(iid, price(raw_of(r[4]), pp), close_ns, ts)


__all__ = [
    "SOURCE_IDS",
    "Converted",
    "archive_url",
    "convert",
    "download",
    "instrument_id_for",
    "stream_for",
]
