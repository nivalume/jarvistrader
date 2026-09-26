#!/usr/bin/env python3
"""Python callback cost on the node (docs/architecture.md section 7.8). Report-only.

Runs a backtest over N quotes three times: with a strategy that defines no callbacks (the node's
own cost per event), with an ``on_quote`` that returns at once (the C++ -> Python call), and with
an ``on_quote`` that records a value (a call back into the kernel). Prints a table and writes
Google Benchmark JSON (``--out``) so the numbers sit next to the C++ ones:

    py/node_event_no_callback   ns per quote, no Python callback defined
    py/callback_on_quote        ns per quote, minus the no-callback run
    py/callback_on_quote_record ns per quote with ctx.record, minus the no-callback run

It needs the jarvis package (an installed wheel, or PYTHONPATH pointing at a build).
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from pathlib import Path

import jarvis
from jarvis import log
from jarvis import model as m

IID = "BTCUSDT-PERP.BINANCE"
DAY = 1_788_220_800_000_000_000


def write_catalog(root: Path, quotes: int) -> Path:
    iid = m.InstrumentId.from_str(IID)
    catalog = root / "catalog"
    with log.EventLogWriter(str(catalog / IID / "bookTicker" / "2026-09-01")) as w:
        for i in range(quotes):
            ts = DAY + i * 1_000_000
            bid = m.Price.from_raw((65_000_0 + i % 7) * 100_000_000, 1)
            ask = m.Price.from_raw((65_000_1 + i % 7) * 100_000_000, 1)
            size = m.Quantity.from_raw(1_000_000_000, 3)
            w.append(m.QuoteTick(iid, bid, ask, size, size, ts, ts), seq=i + 1, ts=ts, source_id=2)
    config = root / "node.toml"
    config.write_text(
        f'[node]\nid = "bench"\n\n[data]\ncatalog = "{catalog}"\n\n'
        f'[[data.streams]]\nvenue = "V"\ninstruments = ["{IID}"]\nstreams = ["bookTicker"]\n\n'
        '[[venues]]\nid = "V"\nkind = "binance_usdm"\n\n[persistence]\nmode = "none"\n'
    )
    return config


class Silent(jarvis.Strategy):
    def on_start(self, ctx: jarvis.Context) -> None:
        ctx.subscribe_quotes(IID)


class Returns(Silent):
    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        return None


class Records(Silent):
    def on_quote(self, ctx: jarvis.Context, quote: m.QuoteTick) -> None:
        ctx.record("bid", quote.bid_price.as_decimal())


def per_event_ns(config: Path, strategy: jarvis.Strategy, quotes: int, repeat: int) -> float:
    best = float("inf")
    for _ in range(repeat):
        node = jarvis.Node(config).add_strategy(strategy)
        start = time.perf_counter_ns()
        result = node.run()
        elapsed = time.perf_counter_ns() - start
        assert result.data_events == quotes, result
        best = min(best, elapsed / quotes)
    return best


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--quotes", type=int, default=200_000)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--out", help="write Google Benchmark JSON here")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="jarvis-bench-") as tmp:
        config = write_catalog(Path(tmp), args.quotes)
        base = per_event_ns(config, Silent(), args.quotes, args.repeat)
        call = per_event_ns(config, Returns(), args.quotes, args.repeat) - base
        record = per_event_ns(config, Records(), args.quotes, args.repeat) - base
    rows = [
        ("py/node_event_no_callback", base),
        ("py/callback_on_quote", call),
        ("py/callback_on_quote_record", record),
    ]
    for name, ns in rows:
        print(f"{name:32s} {ns:10.1f} ns")
    if args.out:
        doc = {
            "context": {"library_build_type": "release", "quotes": args.quotes},
            "benchmarks": [
                {
                    "name": name,
                    "run_name": name,
                    "run_type": "iteration",
                    "iterations": args.quotes,
                    "real_time": ns,
                    "cpu_time": ns,
                    "time_unit": "ns",
                }
                for name, ns in rows
            ],
        }
        Path(args.out).write_text(json.dumps(doc, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
