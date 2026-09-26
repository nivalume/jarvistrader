"""jarvis: a deterministic trading kernel with a Python strategy layer.

Submodules:
    jarvis.model        nautilus-compatible model types (prices, identifiers, data, events)
    jarvis.log          event logs: write, read, fingerprint, compare
    jarvis.determinism  the guard against nondeterministic calls in strategy callbacks
"""

from . import _core as _core
from . import determinism, log, model

build_info = _core.build_info

determinism.install()

__all__ = ["build_info", "determinism", "log", "model"]
