"""Converters from market data archives to catalog logs (docs/architecture.md section 16).

    jarvis.data.binance_vision   data.binance.vision daily archives (aggTrades, bookTicker, ...)
    jarvis.data.parquet          nautilus Parquet catalogs, both directions (needs pyarrow)

A catalog holds one event log per instrument, stream and UTC day,
``{catalog}/{instrument_id}/{stream}/{YYYY-MM-DD}/``, which the backtest node reads through
``[data] catalog`` and ``[[data.streams]]``. ``python -m jarvis.data --help`` runs them from the
command line.
"""

from . import binance_vision

__all__ = ["binance_vision"]
