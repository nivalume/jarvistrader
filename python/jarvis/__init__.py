"""jarvis: a deterministic trading kernel with a Python strategy layer.

Submodules:
    jarvis.model        nautilus-compatible model types (prices, identifiers, data, events)
    jarvis.log          event logs: write, read, fingerprint, compare
    jarvis.determinism  the guard against nondeterministic calls in strategy callbacks
    jarvis.strategy     the Strategy base class, Context, Cadence, book and batch views
    jarvis.features     kernel feature declarations (EMA, VWAP, imbalance, ...)
    jarvis.node         Node, main(): run and replay a node
    jarvis.report       RunReport: fills, fees and PnL of a backtest
"""

import os

from . import _core as _core
from . import determinism, features, log, model
from .node import Node, main
from .report import RunReport
from .strategy import Cadence, Context, DataKind, OrderIntent, OrderView, ParentView, Strategy

build_info = _core.build_info


def load_native(path: str | os.PathLike[str]) -> list[str]:
    """Loads a C++ strategy plugin (built with ``jarvis_add_strategy_plugin`` from the same
    jarvis sources and compiler as this package) and returns the names of its strategies, which
    ``Node.add_native_strategy`` and ``impl = "cpp:<name>"`` can then use."""
    return list(_core.node.load_native(os.fspath(path)))


def registered_strategies() -> list[str]:
    """Names of the C++ strategies this process can create: built in or loaded."""
    return list(_core.node.registered_strategies())


determinism.install()

__all__ = [
    "Cadence",
    "Context",
    "DataKind",
    "Node",
    "OrderIntent",
    "OrderView",
    "ParentView",
    "RunReport",
    "Strategy",
    "build_info",
    "determinism",
    "features",
    "load_native",
    "log",
    "main",
    "model",
    "registered_strategies",
]
