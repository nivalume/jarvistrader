"""Converters: data.binance.vision archives and nautilus Parquet catalogs to catalog logs."""

from __future__ import annotations

import zipfile
from pathlib import Path

import pytest

from jarvis import Cadence, Strategy, log
from jarvis import model as m
from jarvis.data import binance_vision as bv
from jarvis.data.__main__ import main as data_main
from jarvis.data._common import decimals_of, ns_of, raw_of
from jarvis.node import Node

IID = "BTCUSDT-PERP.BINANCE"
T0 = 1_789_862_400_000  # 2026-09-20T00:00:00Z in ms


def _zip(path: Path, name: str, text: str) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(name, text)
    return path


def _agg_trades(tmp_path: Path) -> Path:
    rows = [
        "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker",
        f"100,81225.7,0.007,1,3,{T0 + 2},false",
        f"101,81225.0,0.401,4,9,{T0 + 140},true",
        f"102,81226.1,1.250,10,10,{T0 + 139},false",  # out of order by 1 ms
        f"103,81226.1,0.010,11,11,{T0 + 900},true",
    ]
    return _zip(tmp_path / "BTCUSDT-aggTrades-2026-09-20.zip", "a.csv", "\n".join(rows) + "\n")


def test_decimal_text_helpers() -> None:
    assert raw_of("81225.7") == 81_225_700_000_000
    assert raw_of("-0.000125") == -125_000
    assert raw_of("0.33090000") == 330_900_000
    assert decimals_of("0.33090000") == 4
    assert decimals_of("81225") == 0
    assert ns_of("1789862400002") == 1_789_862_400_002_000_000
    assert ns_of("1789862400002123") == 1_789_862_400_002_123_000  # microseconds (spot, 2025+)
    with pytest.raises(ValueError, match="more than 9 decimals"):
        raw_of("0.1234567891")


def test_agg_trades_become_trade_ticks(tmp_path: Path) -> None:
    r = bv.convert("aggTrades", _agg_trades(tmp_path), tmp_path / "cat", symbol="BTCUSDT")
    assert r.directory == tmp_path / "cat" / IID / "aggTrade" / "2026-09-20"
    assert (r.records, r.price_precision, r.size_precision, r.clamped) == (4, 1, 3, 1)
    records = list(log.read(str(r.directory)))
    trades = [x.event for x in records]
    assert [str(t.price) for t in trades] == ["81225.7", "81225.0", "81226.1", "81226.1"]
    assert [t.aggressor_side for t in trades] == [
        m.AggressorSide.BUY,
        m.AggressorSide.SELL,  # the buyer was the maker: the seller hit the bid
        m.AggressorSide.BUY,
        m.AggressorSide.SELL,
    ]
    assert str(trades[0].trade_id) == "100"
    assert trades[2].ts_event == (T0 + 139) * 1_000_000  # the exchange's time is kept
    assert trades[2].ts_init == (T0 + 140) * 1_000_000  # the key never goes back
    assert [x.seq for x in records] == [1, 2, 3, 4]
    assert {x.source_id for x in records} == {bv.SOURCE_IDS["aggTrades"]}
    with pytest.raises(FileExistsError):
        bv.convert("aggTrades", _agg_trades(tmp_path), tmp_path / "cat", symbol="BTCUSDT")
    again = bv.convert(
        "aggTrades", _agg_trades(tmp_path), tmp_path / "cat", symbol="BTCUSDT",
        price_precision=2, overwrite=True,
    )
    assert str(next(iter(log.read(str(again.directory)))).event.price) == "81225.70"


def test_book_ticker_klines_and_mark_prices(tmp_path: Path) -> None:
    quotes = _zip(
        tmp_path / "q.zip",
        "q.csv",
        f"1,0.33090000,982.60000000,0.33100000,1772.20000000,{T0},{T0 + 5}\n"
        f"2,0.33090000,982.60000000,0.33110000,2073.70000000,{T0 + 1},{T0 + 4}\n",
    )
    r = bv.convert("bookTicker", quotes, tmp_path / "cat", symbol="ZRXUSDT")
    assert (r.price_precision, r.size_precision, r.stream) == (4, 1, "bookTicker")
    q = [x.event for x in log.read(str(r.directory))]
    assert str(q[0].bid_price) == "0.3309" and str(q[1].ask_size) == "2073.7"
    assert q[0].ts_event == T0 * 1_000_000 and q[0].ts_init == (T0 + 5) * 1_000_000
    assert q[1].ts_init == (T0 + 5) * 1_000_000  # event time went back; kept in order

    header = "open_time,open,high,low,close,volume,close_time,quote_volume,count,x,y,ignore\n"
    klines = _zip(
        tmp_path / "k.zip",
        "k.csv",
        header + f"{T0},81225.70,81228.20,81190.70,81199.90,64.573,{T0 + 59999},1,2,3,4,0\n",
    )
    r = bv.convert("klines", klines, tmp_path / "cat", symbol="BTCUSDT")
    [bar] = [x.event for x in log.read(str(r.directory))]
    assert str(bar.bar_type) == f"{IID}-1-MINUTE-LAST-EXTERNAL"
    assert bar.ts_init == (T0 + 60_000) * 1_000_000  # stamped at the close
    assert str(bar.close) == "81199.9" and str(bar.volume) == "64.573"

    marks = _zip(
        tmp_path / "mk.zip",
        "mk.csv",
        header + f"{T0},81225.6,81228.2,81190.7,81212.30366567,0,{T0 + 59999},0,60,0,0,0\n",
    )
    r = bv.convert("markPriceKlines", marks, tmp_path / "cat", symbol="BTCUSDT")
    [mark] = [x.event for x in log.read(str(r.directory))]
    assert isinstance(mark, m.MarkPriceUpdate)
    assert str(mark.value) == "81212.30366567" and r.stream == "markPriceKline_1m"


def test_archive_urls() -> None:
    assert bv.archive_url("aggTrades", "BTCUSDT", "2026-09-20") == (
        "https://data.binance.vision/data/futures/um/daily/aggTrades/BTCUSDT/"
        "BTCUSDT-aggTrades-2026-09-20.zip"
    )
    assert bv.archive_url("klines", "BTCUSDT", "2026-09-20", interval="5m").endswith(
        "/klines/BTCUSDT/5m/BTCUSDT-5m-2026-09-20.zip"
    )
    assert bv.instrument_id_for("BTCUSDT") == IID
    assert bv.instrument_id_for("BTCUSDT", "spot") == "BTCUSDT.BINANCE"


def test_the_command_line_converts_a_local_file(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    code = data_main(
        ["binance-vision", "aggTrades", "--symbol", "BTCUSDT", "--file",
         str(_agg_trades(tmp_path)), "--catalog", str(tmp_path / "cat")]
    )
    assert code == 0
    assert "4 records" in capsys.readouterr().out


class LastPrice(Strategy):
    def on_start(self, ctx: object) -> None:
        ctx.subscribe_trades(IID)  # type: ignore[attr-defined]
        ctx.subscribe_bars(f"{IID}-1-MINUTE-LAST-EXTERNAL", Cadence.EVERY)  # type: ignore[attr-defined]

    def on_trade(self, ctx: object, trade: m.TradeTick) -> None:
        ctx.record("px", trade.price.as_decimal())  # type: ignore[attr-defined]

    def on_bar(self, ctx: object, bar: m.Bar) -> None:
        ctx.record("close", bar.close.as_decimal())  # type: ignore[attr-defined]


def test_a_converted_catalog_runs_in_a_backtest(tmp_path: Path) -> None:
    catalog = tmp_path / "cat"
    bv.convert("aggTrades", _agg_trades(tmp_path), catalog, symbol="BTCUSDT")
    header = "open_time,open,high,low,close,volume,close_time,quote_volume,count,x,y,ignore\n"
    bv.convert(
        "klines",
        _zip(tmp_path / "k.zip", "k.csv", header + f"{T0},1.0,2.0,0.5,1.5,3.000,{T0 + 59999},0,0,0,0,0\n"),
        catalog,
        symbol="BTCUSDT",
    )
    config = tmp_path / "node.toml"
    config.write_text(
        f"""
[node]
id = "bv01"

[data]
catalog = "{catalog}"
range = {{ start = "2026-09-20T00:00:00Z", end = "2026-09-21T00:00:00Z" }}

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["{IID}"]
streams = ["aggTrade", "kline_1m"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"
"""
    )
    result = Node(config, out=tmp_path / "run").add_strategy(LastPrice()).run()
    assert result.data_events == 5
    assert result.outputs == 5
    report = Node.from_run(result.directory).add_strategy(LastPrice()).replay()
    assert report.divergence is None


# ---- nautilus Parquet -------------------------------------------------------------------------


def _deltas_log(catalog: Path) -> None:
    iid = m.InstrumentId.from_str(IID)
    t = T0 * 1_000_000
    batches = []
    for i in range(3):
        orders = [
            m.OrderBookDelta(
                iid, m.BookAction.ADD,
                m.BookOrder(m.OrderSide.BUY, m.Price(f"81225.{i}"), m.Quantity("1.000"), 0),
                0, 10 + i, t + i, t + i,
            ),
            m.OrderBookDelta(
                iid, m.BookAction.UPDATE,
                m.BookOrder(m.OrderSide.SELL, m.Price(f"81226.{i}"), m.Quantity("2.000"), 0),
                128, 10 + i, t + i, t + i,
            ),
        ]
        batches.append(m.OrderBookDeltas(iid, orders))
    with log.EventLogWriter(str(catalog / IID / "depth" / "2026-09-20")) as w:
        for seq, b in enumerate(batches, start=1):
            w.append(b, seq=seq, ts=b.ts_init, source_id=3)


def test_parquet_round_trip_keeps_every_event(tmp_path: Path) -> None:
    pytest.importorskip("pyarrow")
    from jarvis.data import parquet

    catalog = tmp_path / "cat"
    bv.convert("aggTrades", _agg_trades(tmp_path), catalog, symbol="BTCUSDT")
    _deltas_log(catalog)
    written = parquet.from_catalog(catalog, tmp_path / "nt")
    kinds = sorted(w.path.parts[-3] for w in written)
    assert kinds == ["order_book_deltas", "trades"]

    import pyarrow.parquet as pq

    trades_file = next(w.path for w in written if w.path.parts[-3] == "trades")
    table = pq.read_table(trades_file)
    assert str(table.schema.field("price").type) == "decimal128(38, 16)"
    assert str(table.schema.field("ts_init").type) == "timestamp[ns, tz=UTC]"
    assert table.schema.metadata[b"price_precision"] == b"1"
    assert table.column("aggressor_side").to_pylist() == ["BUY", "SELL", "BUY", "SELL"]

    back = parquet.to_catalog(tmp_path / "nt", tmp_path / "cat2")
    assert sorted(w.records for w in back) == [3, 4]

    def events(directory: Path) -> list[str]:
        return [log.event_text(r.event) for r in log.read(str(directory))]

    assert events(tmp_path / "cat2" / IID / "trades" / "2026-09-20") == events(
        catalog / IID / "aggTrade" / "2026-09-20"
    )
    assert events(tmp_path / "cat2" / IID / "order_book_deltas" / "2026-09-20") == events(
        catalog / IID / "depth" / "2026-09-20"
    )


def test_parquet_reads_legacy_raw_encodings(tmp_path: Path) -> None:
    pa = pytest.importorskip("pyarrow")
    import pyarrow.parquet as pq

    from jarvis.data import parquet

    path = tmp_path / "nt" / "data" / "trade_tick" / IID / "legacy.parquet"
    path.parent.mkdir(parents=True)
    table = pa.table(
        {
            "price": pa.array([81_225_700_000_000], pa.int64()),
            "size": pa.array([(7_000_000).to_bytes(8, "little")], pa.binary(8)),
            "aggressor_side": pa.array(["BUYER"]),
            "trade_id": pa.array(["1"]),
            "ts_event": pa.array([5], pa.uint64()),
            "ts_init": pa.array([6], pa.uint64()),
        }
    ).replace_schema_metadata(
        {"instrument_id": IID, "price_precision": "1", "size_precision": "3"}
    )
    pq.write_table(table, path)
    [trade] = parquet.read_file(path, "trade_tick")
    assert str(trade.price) == "81225.7" and str(trade.size) == "0.007"
    assert trade.aggressor_side == m.AggressorSide.BUY
    assert (trade.ts_event, trade.ts_init) == (5, 6)
