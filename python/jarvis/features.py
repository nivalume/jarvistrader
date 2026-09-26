"""Kernel features (docs/architecture.md section 7.5).

Features are computed inside the kernel in fixed point and delivered as ``on_feature(ctx,
feature_id, value, ts)`` at the cadence the strategy asks for::

    self.mid = ctx.feature(features.microprice("BTCUSDT-PERP.BINANCE"), Cadence.sampled_ms(100))

Identical declarations share one feature. Values are ``decimal.Decimal``; each delivered value
is also written to the run log and checked by replay.
"""

from __future__ import annotations

from typing import Any

from ._core import node as _native

FeatureKind = _native.FeatureKind
FeatureSpec = _native.FeatureSpec


def ema(instrument_id: Any, window: int) -> FeatureSpec:
    """Exponential moving average of trade prices over ``window`` trades."""
    return FeatureSpec(FeatureKind.EMA, instrument_id, window)


def vwap(instrument_id: Any, window: int) -> FeatureSpec:
    """Volume-weighted average price of the last ``window`` trades."""
    return FeatureSpec(FeatureKind.VWAP, instrument_id, window)


def imbalance(instrument_id: Any) -> FeatureSpec:
    """(bid size - ask size) / (bid size + ask size) at the top of the book, from quotes."""
    return FeatureSpec(FeatureKind.IMBALANCE, instrument_id)


def microprice(instrument_id: Any) -> FeatureSpec:
    """Size-weighted mid price at the top of the book, from quotes."""
    return FeatureSpec(FeatureKind.MICROPRICE, instrument_id)


def realized_vol(instrument_id: Any, window: int) -> FeatureSpec:
    """Square root of the sum of squared simple returns over the last ``window`` trades."""
    return FeatureSpec(FeatureKind.REALIZED_VOL, instrument_id, window)


__all__ = ["FeatureKind", "FeatureSpec", "ema", "imbalance", "microprice", "realized_vol", "vwap"]
