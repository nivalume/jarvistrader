"""nautilus Parquet catalogs to catalog logs and back (docs/architecture.md section 16.5).

Needs ``pyarrow`` (``pip install jarvis[parquet]``).

Layout and schema follow nautilus_trader at cd417b80 (``crates/serialization/src/arrow``):
``{root}/data/{type}/{identifier}/{start}_{end}.parquet`` with ``type`` one of ``trades``,
``quotes``, ``bars``, ``order_book_deltas``, ``mark_prices``, ``index_prices`` (the legacy names
``trade_tick``, ``quote_tick``, ``bar``, ``order_book_delta``, ``mark_price_update`` and
``index_price_update`` are read too). Prices and sizes are ``Decimal128(38, 16)``, timestamps
``Timestamp(ns, UTC)``, enums ``Dictionary(Int8, Utf8)`` with their nautilus names, plus a
nullable ``identifier`` column; the schema metadata carries ``instrument_id`` (``bar_type`` for
bars), ``price_precision`` and ``size_precision``. On read, prices and sizes may also be the
legacy raw encodings (``Int64`` or ``FixedSizeBinary(8)`` at 10^9, ``FixedSizeBinary(16)`` at
10^16) and timestamps ``UInt64``. File names are ``{first ts_init}_{last ts_init}`` in
nanoseconds.

The schema was written from the nautilus source; round trips are tested here, not against a
running nautilus.

A nautilus order book file holds single deltas; they are grouped into OrderBookDeltas up to and
including each delta flagged F_LAST (128), and split back into single deltas on write.
"""

from __future__ import annotations

import os
from collections import defaultdict
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import log
from .. import model as m
from ._common import MonotonicClock, catalog_dir, utc_day, write_log

F_LAST = 128
DECIMAL_SCALE = 16
_SCALE_UP = 10 ** (DECIMAL_SCALE - 9)

# nautilus type directory -> (our stream name, source id)
TYPES = {
    "trades": ("trades", 11),
    "quotes": ("quotes", 12),
    "bars": ("bars", 13),
    "order_book_deltas": ("order_book_deltas", 14),
    "mark_prices": ("mark_prices", 15),
    "index_prices": ("index_prices", 16),
}
_LEGACY = {
    "trade_tick": "trades",
    "quote_tick": "quotes",
    "bar": "bars",
    "order_book_delta": "order_book_deltas",
    "mark_price_update": "mark_prices",
    "index_price_update": "index_prices",
}
_KIND_TYPES = {
    "TradeTick": "trades",
    "QuoteTick": "quotes",
    "Bar": "bars",
    "OrderBookDeltas": "order_book_deltas",
    "MarkPriceUpdate": "mark_prices",
    "IndexPriceUpdate": "index_prices",
}


def _pa() -> Any:
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
    except ImportError as e:  # pragma: no cover - depends on the environment
        raise ImportError("Parquet conversion needs pyarrow: pip install 'jarvis[parquet]'") from e
    return pa, pq


@dataclass(frozen=True)
class Written:
    path: Path
    records: int


# ---- reading --------------------------------------------------------------------------------


def _raw_column(column: Any, precision_scale: int = 9) -> list[int]:
    """Raw values at 10^9 from a Decimal128, Int64 or FixedSizeBinary column."""
    pa, _ = _pa()
    column = column.combine_chunks() if hasattr(column, "combine_chunks") else column
    t = column.type
    if pa.types.is_decimal(t):
        buf = column.buffers()[1]
        data = memoryview(buf)[column.offset * 16 : (column.offset + len(column)) * 16]
        shift = 10 ** (t.scale - 9) if t.scale >= 9 else None
        out = []
        for i in range(len(column)):
            value = int.from_bytes(data[i * 16 : i * 16 + 16], "little", signed=True)
            if shift is None:
                out.append(value * 10 ** (9 - t.scale))
            else:
                q, r = divmod(value, shift)
                if r:
                    raise ValueError(f"value at row {i} has more than 9 decimals")
                out.append(q)
        return out
    if pa.types.is_integer(t):
        return [int(v) for v in column.to_numpy(zero_copy_only=False)]
    if pa.types.is_fixed_size_binary(t):
        width = t.byte_width
        values = [int.from_bytes(v, "little", signed=True) for v in column.to_pylist()]
        if width == 16:  # high-precision raw at 10^16
            return [v // _SCALE_UP for v in values]
        return values
    raise ValueError(f"unsupported price/size column type {t}")


def _ns_column(column: Any) -> list[int]:
    pa, _ = _pa()
    column = column.combine_chunks() if hasattr(column, "combine_chunks") else column
    if pa.types.is_timestamp(column.type):
        column = column.cast(pa.int64())
    return [int(v) for v in column.to_numpy(zero_copy_only=False)]


def _text_column(column: Any) -> list[str]:
    return [str(v) for v in column.to_pylist()]


def _meta(table: Any) -> dict[str, str]:
    raw = table.schema.metadata or {}
    return {k.decode(): v.decode() for k, v in raw.items()}


def read_file(path: str | os.PathLike[str], type_name: str, identifier: str = "") -> list[object]:
    """The events of one nautilus Parquet file (type as in TYPES, legacy names accepted)."""
    _, pq = _pa()
    kind = _LEGACY.get(type_name, type_name)
    table = pq.read_table(str(path))
    meta = _meta(table)
    n = table.num_rows
    if n == 0:
        return []
    pp = int(meta.get("price_precision", "0"))
    sp = int(meta.get("size_precision", "0"))
    col = table.column
    ts_event = _ns_column(col("ts_event"))
    ts_init = _ns_column(col("ts_init"))
    if kind == "bars":
        bar_type = m.BarType.from_str(meta.get("bar_type") or identifier)
        o, h, lo, c = (_raw_column(col(x)) for x in ("open", "high", "low", "close"))
        v = _raw_column(col("volume"))
        return [
            m.Bar(
                bar_type, m.Price.from_raw(o[i], pp), m.Price.from_raw(h[i], pp),
                m.Price.from_raw(lo[i], pp), m.Price.from_raw(c[i], pp),
                m.Quantity.from_raw(v[i], sp), ts_event[i], ts_init[i],
            )
            for i in range(n)
        ]
    iid = m.InstrumentId.from_str(meta.get("instrument_id") or identifier)
    if kind == "trades":
        px, sz = _raw_column(col("price")), _raw_column(col("size"))
        side = [m.AggressorSide.from_str(s) for s in _text_column(col("aggressor_side"))]
        tid = _text_column(col("trade_id"))
        return [
            m.TradeTick(
                iid, m.Price.from_raw(px[i], pp), m.Quantity.from_raw(sz[i], sp), side[i],
                m.TradeId(tid[i]), ts_event[i], ts_init[i],
            )
            for i in range(n)
        ]
    if kind == "quotes":
        bp, ap = _raw_column(col("bid_price")), _raw_column(col("ask_price"))
        bs, az = _raw_column(col("bid_size")), _raw_column(col("ask_size"))
        return [
            m.QuoteTick(
                iid, m.Price.from_raw(bp[i], pp), m.Price.from_raw(ap[i], pp),
                m.Quantity.from_raw(bs[i], sp), m.Quantity.from_raw(az[i], sp),
                ts_event[i], ts_init[i],
            )
            for i in range(n)
        ]
    if kind in ("mark_prices", "index_prices"):
        value = _raw_column(col("value"))
        cls = m.MarkPriceUpdate if kind == "mark_prices" else m.IndexPriceUpdate
        return [cls(iid, m.Price.from_raw(value[i], pp), ts_event[i], ts_init[i]) for i in range(n)]
    if kind == "order_book_deltas":
        return _read_deltas(table, iid, pp, sp, ts_event, ts_init)
    raise ValueError(f"unsupported nautilus data type {type_name!r}")


def _read_deltas(
    table: Any, iid: m.InstrumentId, pp: int, sp: int, ts_event: list[int], ts_init: list[int]
) -> list[object]:
    col = table.column
    action = [m.BookAction.from_str(s) for s in _text_column(col("action"))]
    side_text = _text_column(col("side"))
    px, sz = _raw_column(col("price")), _raw_column(col("size"))
    order_id = [int(v) for v in col("order_id").to_pylist()]
    flags = [int(v) for v in col("flags").to_pylist()]
    sequence = [int(v) for v in col("sequence").to_pylist()]
    out: list[object] = []
    batch: list[m.OrderBookDelta] = []
    for i in range(table.num_rows):
        side = m.OrderSide.SELL if side_text[i] in ("SELL", "SELLER") else m.OrderSide.BUY
        order = m.BookOrder(side, m.Price.from_raw(px[i], pp), m.Quantity.from_raw(sz[i], sp), order_id[i])
        batch.append(
            m.OrderBookDelta(iid, action[i], order, flags[i], sequence[i], ts_event[i], ts_init[i])
        )
        if flags[i] & F_LAST:
            out.append(m.OrderBookDeltas(iid, batch))
            batch = []
    if batch:
        out.append(m.OrderBookDeltas(iid, batch))
    return out


def _nautilus_files(root: Path) -> Iterator[tuple[str, str, Path]]:
    data = root / "data"
    for type_dir in sorted(p for p in data.iterdir() if p.is_dir()):
        kind = _LEGACY.get(type_dir.name, type_dir.name)
        if kind not in TYPES:
            continue
        for path in sorted(type_dir.rglob("*.parquet")):
            identifier = path.parent.name if path.parent != type_dir else ""
            yield type_dir.name, identifier, path


def to_catalog(
    root: str | os.PathLike[str],
    catalog: str | os.PathLike[str],
    *,
    overwrite: bool = False,
) -> list[Written]:
    """Converts every supported file under ``{root}/data`` into catalog logs, one per
    (instrument or bar type, type, UTC day)."""
    groups: dict[tuple[str, str, str], list[object]] = defaultdict(list)
    for type_name, identifier, path in _nautilus_files(Path(root)):
        kind = _LEGACY.get(type_name, type_name)
        for event in read_file(path, type_name, identifier):
            iid, stream = _placement(kind, event)
            groups[(iid, stream, kind)].append(event)
    written = []
    for (iid, stream, kind), events in sorted(groups.items()):
        events.sort(key=lambda e: e.ts_init)
        by_day: dict[str, list[object]] = defaultdict(list)
        for e in events:
            by_day[utc_day(e.ts_init)].append(e)
        for day, day_events in sorted(by_day.items()):
            directory = catalog_dir(catalog, iid, stream, day)
            clock = MonotonicClock()
            pairs = ((clock(e.ts_init), e) for e in day_events)
            count = write_log(directory, pairs, source_id=TYPES[kind][1], overwrite=overwrite)
            written.append(Written(directory, count))
    return written


def _placement(kind: str, event: Any) -> tuple[str, str]:
    """(instrument id, stream) of an event in the catalog."""
    if kind == "bars":
        text = str(event.bar_type)
        iid = str(event.bar_type.instrument_id)
        return iid, "bars-" + text[len(iid) + 1 :]
    return str(event.instrument_id), TYPES[kind][0]


# ---- writing --------------------------------------------------------------------------------


def _decimal_array(raws: Sequence[int]) -> Any:
    pa, _ = _pa()
    data = b"".join((r * _SCALE_UP).to_bytes(16, "little", signed=True) for r in raws)
    return pa.Array.from_buffers(pa.decimal128(38, DECIMAL_SCALE), len(raws), [None, pa.py_buffer(data)])


def _ts_array(values: Sequence[int]) -> Any:
    pa, _ = _pa()
    return pa.array(values, type=pa.int64()).cast(pa.timestamp("ns", tz="UTC"))


def _enum_array(names: Sequence[str]) -> Any:
    pa, _ = _pa()
    return pa.array(names, type=pa.string()).dictionary_encode().cast(pa.dictionary(pa.int8(), pa.string()))


def _table(kind: str, events: Sequence[Any]) -> tuple[Any, str]:
    pa, _ = _pa()
    first = events[0]
    cols: dict[str, Any] = {}
    if kind == "trades":
        meta = _meta_for(str(first.instrument_id), first.price.precision, first.size.precision)
        cols["price"] = _decimal_array([e.price.raw for e in events])
        cols["size"] = _decimal_array([e.size.raw for e in events])
        cols["aggressor_side"] = _enum_array([e.aggressor_side.name for e in events])
        cols["trade_id"] = pa.array([str(e.trade_id) for e in events], type=pa.string())
        identifier = str(first.instrument_id)
    elif kind == "quotes":
        meta = _meta_for(str(first.instrument_id), first.bid_price.precision, first.bid_size.precision)
        for f in ("bid_price", "ask_price", "bid_size", "ask_size"):
            cols[f] = _decimal_array([getattr(e, f).raw for e in events])
        identifier = str(first.instrument_id)
    elif kind == "bars":
        meta = {
            "bar_type": str(first.bar_type),
            "price_precision": str(first.open.precision),
            "size_precision": str(first.volume.precision),
        }
        for f in ("open", "high", "low", "close", "volume"):
            cols[f] = _decimal_array([getattr(e, f).raw for e in events])
        identifier = str(first.bar_type)
    elif kind in ("mark_prices", "index_prices"):
        meta = {"instrument_id": str(first.instrument_id), "price_precision": str(first.value.precision)}
        cols["value"] = _decimal_array([e.value.raw for e in events])
        identifier = str(first.instrument_id)
    else:
        return _deltas_table(events)
    cols["ts_event"] = _ts_array([e.ts_event for e in events])
    cols["ts_init"] = _ts_array([e.ts_init for e in events])
    cols["identifier"] = pa.array([identifier] * len(events), type=pa.string())
    table = pa.table(cols).replace_schema_metadata(meta)
    return table, identifier


def _deltas_table(batches: Sequence[Any]) -> tuple[Any, str]:
    pa, _ = _pa()
    deltas = [d for b in batches for d in b.deltas]
    first = deltas[0]
    identifier = str(first.instrument_id)
    meta = _meta_for(identifier, first.order.price.precision, first.order.size.precision)
    cols = {
        "action": _enum_array([d.action.name for d in deltas]),
        "side": _enum_array(
            ["NO_ORDER_SIDE" if d.action == m.BookAction.CLEAR else d.order.side.name for d in deltas]
        ),
        "price": _decimal_array([d.order.price.raw for d in deltas]),
        "size": _decimal_array([d.order.size.raw for d in deltas]),
        "order_id": pa.array([d.order.order_id for d in deltas], type=pa.uint64()),
        "flags": pa.array([d.flags for d in deltas], type=pa.uint8()),
        "sequence": pa.array([d.sequence for d in deltas], type=pa.uint64()),
        "ts_event": _ts_array([d.ts_event for d in deltas]),
        "ts_init": _ts_array([d.ts_init for d in deltas]),
        "identifier": pa.array([identifier] * len(deltas), type=pa.string()),
    }
    return pa.table(cols).replace_schema_metadata(meta), identifier


def _meta_for(instrument_id: str, price_precision: int, size_precision: int) -> dict[str, str]:
    return {
        "instrument_id": instrument_id,
        "price_precision": str(price_precision),
        "size_precision": str(size_precision),
    }


def from_catalog(
    catalog: str | os.PathLike[str],
    root: str | os.PathLike[str],
    *,
    overwrite: bool = False,
) -> list[Written]:
    """Writes every catalog log (inputs only) as nautilus Parquet files under ``{root}/data``."""
    _, pq = _pa()
    written = []
    for directory in sorted(p.parent for p in Path(catalog).rglob("events-000000.jlog")):
        by_kind: dict[str, list[object]] = defaultdict(list)
        for record in log.read(str(directory)):
            kind = _KIND_TYPES.get(record.kind)
            if kind is not None and not record.is_output:
                by_kind[kind].append(record.event)
        for kind, events in sorted(by_kind.items()):
            table, identifier = _table(kind, events)
            first, last = events[0].ts_init, events[-1].ts_init
            path = Path(root) / "data" / kind / identifier / f"{first}_{last}.parquet"
            if path.exists() and not overwrite:
                raise FileExistsError(f"{path} exists; pass overwrite=True to replace it")
            path.parent.mkdir(parents=True, exist_ok=True)
            pq.write_table(table, str(path))
            written.append(Written(path, len(events)))
    return written


__all__ = ["TYPES", "Written", "from_catalog", "read_file", "to_catalog"]
