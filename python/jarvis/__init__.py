"""jarvis: a deterministic trading kernel with a Python strategy layer.

Submodules:
    jarvis.model        nautilus-compatible model types (prices, identifiers, data, events)
    jarvis.log          event logs: write, read, fingerprint, compare
    jarvis.determinism  the guard against nondeterministic calls in strategy callbacks
    jarvis.strategy     the Strategy base class, Context, Cadence, book and batch views
    jarvis.features     kernel feature declarations (EMA, VWAP, imbalance, ...)
    jarvis.node         Node, main(): run and replay a node
"""

from . import _core as _core
from . import determinism, features, log, model
from .node import Node, main
from .strategy import Cadence, Context, DataKind, Strategy

build_info = _core.build_info

determinism.install()

__all__ = [
    "Cadence",
    "Context",
    "DataKind",
    "Node",
    "Strategy",
    "build_info",
    "determinism",
    "features",
    "log",
    "main",
    "model",
]
