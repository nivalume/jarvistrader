"""Command line for the converters.

    python -m jarvis.data binance-vision DATASET --symbol S --start D [--end D] --catalog DIR
        [--market um|spot] [--interval 1m] [--download-dir DIR | --file PATH]
        [--price-precision N] [--size-precision N] [--overwrite]
    python -m jarvis.data binance-instrument --symbol S --day D --catalog DIR
        (--exchange-info FILE | --tick T --step S [--min-qty Q] [--max-qty Q]
         [--min-notional N] [--margin-init R] [--margin-maint R]) [--overwrite]
    python -m jarvis.data parquet-to-catalog ROOT --catalog DIR [--overwrite]
    python -m jarvis.data catalog-to-parquet CATALOG --root DIR [--overwrite]
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

from . import binance_vision


def _days(start: str, end: str | None) -> list[str]:
    first = dt.date.fromisoformat(start)
    last = dt.date.fromisoformat(end) if end else first
    if last < first:
        raise ValueError("--end is before --start")
    return [(first + dt.timedelta(days=i)).isoformat() for i in range((last - first).days + 1)]


def _binance(args: argparse.Namespace) -> int:
    if args.file:
        sources = [(None, Path(args.file))]
    else:
        download_dir = Path(args.download_dir or tempfile.mkdtemp(prefix="jarvis-bv-"))
        sources = []
        for day in _days(args.start, args.end):
            path = binance_vision.download(
                args.dataset, args.symbol, day, download_dir, market=args.market, interval=args.interval
            )
            sources.append((day, path))
    for _, path in sources:
        r = binance_vision.convert(
            args.dataset,
            path,
            args.catalog,
            symbol=args.symbol,
            market=args.market,
            interval=args.interval,
            price_precision=args.price_precision,
            size_precision=args.size_precision,
            overwrite=args.overwrite,
        )
        note = f", {r.clamped} timestamps raised to keep order" if r.clamped else ""
        print(
            f"{r.directory}: {r.records} records, price precision {r.price_precision}, "
            f"size precision {r.size_precision}{note}"
        )
    return 0


def _instrument(args: argparse.Namespace) -> int:
    from . import binance_instrument as bi

    ts = int(dt.datetime.fromisoformat(f"{args.day}T00:00:00+00:00").timestamp()) * 1_000_000_000
    if args.exchange_info:
        instrument = bi.from_exchange_info(bi.load_exchange_info(args.exchange_info), args.symbol, ts=ts)
    else:
        if not args.tick or not args.step:
            raise ValueError("binance-instrument needs --exchange-info, or --tick and --step")
        instrument = bi.perpetual(
            args.symbol,
            tick=args.tick,
            step=args.step,
            min_qty=args.min_qty,
            max_qty=args.max_qty,
            min_notional=args.min_notional,
            margin_init=args.margin_init,
            margin_maint=args.margin_maint,
            ts=ts,
        )
    directory = bi.write(instrument, args.catalog, args.day, overwrite=args.overwrite)
    print(f"{directory}: {instrument.id}")
    return 0


def _parquet_in(args: argparse.Namespace) -> int:
    from . import parquet

    for w in parquet.to_catalog(args.root, args.catalog, overwrite=args.overwrite):
        print(f"{w.path}: {w.records} records")
    return 0


def _parquet_out(args: argparse.Namespace) -> int:
    from . import parquet

    for w in parquet.from_catalog(args.catalog, args.root, overwrite=args.overwrite):
        print(f"{w.path}: {w.records} records")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m jarvis.data", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    bv = sub.add_parser("binance-vision", help="data.binance.vision daily archives to catalog logs")
    bv.add_argument("dataset", choices=sorted(binance_vision.SOURCE_IDS))
    bv.add_argument("--symbol", required=True)
    bv.add_argument("--start", help="first UTC day, YYYY-MM-DD (with --download-dir or alone)")
    bv.add_argument("--end", help="last UTC day, inclusive (default: --start)")
    bv.add_argument("--catalog", required=True)
    bv.add_argument("--market", choices=["um", "spot"], default="um")
    bv.add_argument("--interval", default="1m")
    bv.add_argument("--download-dir")
    bv.add_argument("--file", help="convert this local .zip or .csv instead of downloading")
    bv.add_argument("--price-precision", type=int)
    bv.add_argument("--size-precision", type=int)
    bv.add_argument("--overwrite", action="store_true")
    bv.set_defaults(run=_binance)

    bi = sub.add_parser("binance-instrument", help="a USD-M perpetual's definition for the catalog")
    bi.add_argument("--symbol", required=True)
    bi.add_argument("--day", required=True, help="UTC day the definition is valid from")
    bi.add_argument("--catalog", required=True)
    bi.add_argument("--exchange-info", help="a saved /fapi/v1/exchangeInfo response")
    bi.add_argument("--tick", help="PRICE_FILTER tickSize")
    bi.add_argument("--step", help="LOT_SIZE stepSize")
    bi.add_argument("--min-qty")
    bi.add_argument("--max-qty")
    bi.add_argument("--min-notional", help="MIN_NOTIONAL notional, in the quote currency")
    bi.add_argument("--margin-init", default="0.05")
    bi.add_argument("--margin-maint", default="0.025")
    bi.add_argument("--overwrite", action="store_true")
    bi.set_defaults(run=_instrument)

    pin = sub.add_parser("parquet-to-catalog", help="nautilus Parquet catalog to catalog logs")
    pin.add_argument("root")
    pin.add_argument("--catalog", required=True)
    pin.add_argument("--overwrite", action="store_true")
    pin.set_defaults(run=_parquet_in)

    pout = sub.add_parser("catalog-to-parquet", help="catalog logs to a nautilus Parquet catalog")
    pout.add_argument("catalog")
    pout.add_argument("--root", required=True)
    pout.add_argument("--overwrite", action="store_true")
    pout.set_defaults(run=_parquet_out)

    args = parser.parse_args(argv)
    if args.command == "binance-vision" and not args.file and not args.start:
        parser.error("binance-vision needs --start (to download) or --file")
    try:
        return int(args.run(args))
    except (ValueError, OSError) as e:
        print(f"python -m jarvis.data: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
