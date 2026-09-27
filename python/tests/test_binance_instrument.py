"""Instrument definitions for the catalog: from filters and from a saved exchangeInfo."""

from __future__ import annotations

from pathlib import Path

from jarvis import log
from jarvis import model as m
from jarvis.data import __main__ as cli
from jarvis.data import binance_instrument as bi

INFO = {
    "symbols": [
        {
            "symbol": "BTCUSDT",
            "contractType": "PERPETUAL",
            "baseAsset": "BTC",
            "quoteAsset": "USDT",
            "marginAsset": "USDT",
            "maintMarginPercent": "2.5000",
            "requiredMarginPercent": "5.0000",
            "filters": [
                {"filterType": "PRICE_FILTER", "minPrice": "556.80", "maxPrice": "4529764",
                 "tickSize": "0.10"},
                {"filterType": "LOT_SIZE", "minQty": "0.001", "maxQty": "1000", "stepSize": "0.001"},
                {"filterType": "MIN_NOTIONAL", "notional": "100"},
            ],
        }
    ]
}


def test_a_perpetual_from_exchange_info() -> None:
    p = bi.from_exchange_info(INFO, "BTCUSDT", ts=5)
    assert str(p.id) == "BTCUSDT-PERP.BINANCE"
    assert p.price_precision == 1 and p.size_precision == 3
    assert str(p.price_increment) == "0.1"
    assert str(p.size_increment) == "0.001"
    assert str(p.min_quantity) == "0.001"
    assert str(p.max_quantity) == "1000.000"
    assert str(p.min_price) == "556.8"
    assert str(p.min_notional) == "100.00000000 USDT"
    assert str(p.margin_init) == "0.05"
    assert str(p.margin_maint) == "0.025"


def test_the_cli_writes_the_catalog_stream(tmp_path: Path) -> None:
    catalog = tmp_path / "catalog"
    assert cli.main(["binance-instrument", "--symbol", "BTCUSDT", "--day", "2024-03-30",
                     "--catalog", str(catalog), "--tick", "0.10", "--step", "0.001",
                     "--min-notional", "100"]) == 0
    directory = catalog / "BTCUSDT-PERP.BINANCE" / "instrument" / "2024-03-30"
    records = list(log.read(str(directory)))
    assert [r.kind for r in records] == ["CryptoPerpetual"]
    assert records[0].ts == 1_711_756_800_000_000_000
    assert isinstance(records[0].event, m.CryptoPerpetual)
    assert cli.main(["binance-instrument", "--symbol", "BTCUSDT", "--day", "2024-03-30",
                     "--catalog", str(catalog)]) != 0  # neither --exchange-info nor filters


DATA = Path(__file__).resolve().parents[2] / "tests" / "data" / "binance"
FIELDS = [
    "id", "price_precision", "size_precision", "price_increment", "size_increment", "min_price",
    "max_price", "min_quantity", "max_quantity", "min_notional", "margin_init", "margin_maint",
    "base_currency", "quote_currency", "settlement_currency",
]


def test_the_catalog_mapping_matches_the_live_loader_fixture() -> None:
    """The C++ exchangeInfo loader is checked against the same file (tests/cpp/test_binance_adapter.cpp),
    so live nodes and backtest catalogs define an instrument identically."""
    info = bi.load_exchange_info(DATA / "exchange_info_testnet.json")
    expected = [
        line for line in (DATA / "exchange_info_expected.txt").read_text().splitlines()
        if line and not line.startswith("#")
    ]
    got = []
    for line in expected:
        symbol = line.split(" ", 1)[0]
        inst = bi.from_exchange_info(info, symbol)
        got.append(symbol + " " + " ".join(f"{f}={getattr(inst, f)}" for f in FIELDS))
    assert got == expected
