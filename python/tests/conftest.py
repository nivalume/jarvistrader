"""Shared test settings: which native build the tests run against.

The same Python package runs on the C++ extension and on the Rust one (docs/rust-plan.md R4).
The Rust tree lags the C++ one in two places, and the tests say so instead of failing: it has
no live layer yet (R5), so its run-log model lacks the live inputs, and no execution algorithms
yet (R3, second step), so ``ctx.submit_parent`` raises.
"""

from __future__ import annotations

from jarvis._core import node as _native

import jarvis

RUST = jarvis.build_info()["compiler"].startswith("rustc")
HAS_EXEC_ALGOS = bool(getattr(_native, "has_exec_algos", True))
# Record kinds the build's corpus writes: every event kind of its model.
RECORD_KINDS = 37 if RUST else 42
