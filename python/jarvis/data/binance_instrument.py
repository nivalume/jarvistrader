"""Instrument definitions for the catalog (``{catalog}/{iid}/instrument/{day}/``).

The simulated venue and the risk checks need the instrument's filters, which the
data.binance.vision archives do not carry. They come from the venue's exchangeInfo: either a
saved ``/fapi/v1/exchangeInfo`` response (``from_exchange_info``) or the filter values given
explicitly (``perpetual``). The live adapter loads them itself (plan.md M4).
"""

from __future__ import annotations

import datetime as dt
import json
import os
from decimal import Decimal
from pathlib import Path
from typing import Any

from .. import model as m
from ._common import catalog_dir, write_log
from .binance_vision import instrument_id_for

SOURCE_ID = 9  # the catalog's instrument streams


def _decimals(text: str) -> int:
    d = Decimal(text).normalize()
    exponent = d.as_tuple().exponent
    assert isinstance(exponent, int)
    return max(0, -exponent)


def _plain(text: str) -> str:
    """"0.10000000" -> "0.1"; keeps at least one digit."""
    d = Decimal(text).normalize()
    return format(d, "f")


def perpetual(
    symbol: str,
    *,
    tick: str,
    step: str,
    min_qty: str | None = None,
    max_qty: str | None = None,
    min_price: str | None = None,
    max_price: str | None = None,
    min_notional: str | None = None,
    margin_init: str = "0.05",
    margin_maint: str = "0.025",
    base: str | None = None,
    quote: str = "USDT",
    ts: int = 0,
) -> m.CryptoPerpetual:
    """A USDⓈ-M perpetual from its filters (PRICE_FILTER, LOT_SIZE, MIN_NOTIONAL)."""
    base = base or symbol.removesuffix(quote)
    tick, step = _plain(tick), _plain(step)
    price_precision, size_precision = _decimals(tick), _decimals(step)
    kwargs: dict[str, Any] = dict(
        id=m.InstrumentId.from_str(instrument_id_for(symbol, "um")),
        raw_symbol=m.Symbol(symbol),
        base_currency=m.Currency.from_str(base),
        quote_currency=m.Currency.from_str(quote),
        settlement_currency=m.Currency.from_str(quote),
        price_precision=price_precision,
        size_precision=size_precision,
        price_increment=m.Price(tick),
        size_increment=m.Quantity(step),
        multiplier=m.Quantity("1"),
        margin_init=Decimal(margin_init),
        margin_maint=Decimal(margin_maint),
        ts_event=ts,
        ts_init=ts,
    )
    if min_qty is not None:
        kwargs["min_quantity"] = m.Quantity.from_str(_quantize(min_qty, size_precision))
    if max_qty is not None:
        kwargs["max_quantity"] = m.Quantity.from_str(_quantize(max_qty, size_precision))
    if min_price is not None:
        kwargs["min_price"] = m.Price.from_str(_quantize(min_price, price_precision))
    if max_price is not None:
        kwargs["max_price"] = m.Price.from_str(_quantize(max_price, price_precision))
    if min_notional is not None:
        kwargs["min_notional"] = m.Money.from_str(f"{_plain(min_notional)} {quote}")
    return m.CryptoPerpetual(**kwargs)


def _quantize(text: str, precision: int) -> str:
    return str(Decimal(text).quantize(Decimal(1).scaleb(-precision)))


def from_exchange_info(info: dict[str, Any], symbol: str, *, ts: int = 0) -> m.CryptoPerpetual:
    """The perpetual `symbol` of a saved /fapi/v1/exchangeInfo response."""
    for s in info.get("symbols", []):
        if s.get("symbol") != symbol:
            continue
        if s.get("contractType") != "PERPETUAL":
            raise ValueError(f"{symbol} is a {s.get('contractType')} contract, not a perpetual")
        filters = {f["filterType"]: f for f in s.get("filters", [])}
        price_filter = filters["PRICE_FILTER"]
        lot = filters["LOT_SIZE"]
        notional = filters.get("MIN_NOTIONAL", {}).get("notional")
        maint = s.get("maintMarginPercent", "2.5")
        initial = s.get("requiredMarginPercent", "5")
        return perpetual(
            symbol,
            tick=price_filter["tickSize"],
            step=lot["stepSize"],
            min_qty=lot.get("minQty"),
            max_qty=lot.get("maxQty"),
            min_price=price_filter.get("minPrice"),
            max_price=price_filter.get("maxPrice"),
            min_notional=notional,
            margin_init=_plain(str(Decimal(initial) / 100)),
            margin_maint=_plain(str(Decimal(maint) / 100)),
            base=s.get("baseAsset"),
            quote=s.get("marginAsset", s.get("quoteAsset", "USDT")),
            ts=ts,
        )
    raise ValueError(f"{symbol} is not in the exchangeInfo response")


def write(
    instrument: m.CryptoPerpetual,
    catalog: str | os.PathLike[str],
    day: str,
    *,
    overwrite: bool = False,
) -> Path:
    """Writes the definition as the catalog's instrument stream for `day` (valid from 00:00)."""
    ts = int(dt.datetime.fromisoformat(f"{day}T00:00:00+00:00").timestamp()) * 1_000_000_000
    directory = catalog_dir(catalog, str(instrument.id), "instrument", day)
    write_log(directory, [(ts, instrument)], source_id=SOURCE_ID, overwrite=overwrite)
    return directory


def load_exchange_info(path: str | os.PathLike[str]) -> dict[str, Any]:
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path} is not an exchangeInfo response")
    return data
