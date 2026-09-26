"""Guard against nondeterministic calls inside strategy callbacks (docs/architecture.md 7.7).

Strategy code runs inside the kernel's ``step``, so it must be a function of the events it
receives. While ``guard()`` is active, reading the wall clock, drawing from ``random``, making
random UUIDs or reading OS entropy raises ``NondeterminismError``; the legitimate sources are the
context's clock and counter-based RNG. Outside the guard every function behaves normally.

Coverage: ``install()`` (run when ``jarvis`` is imported) replaces the module attributes
``time.time``, ``time.time_ns``, ``time.monotonic``, ``time.monotonic_ns``,
``time.perf_counter``, ``time.perf_counter_ns``, ``time.process_time``, ``time.localtime``,
``time.gmtime``, ``time.ctime``, ``time.strftime``, ``random.*``, ``uuid.uuid1``, ``uuid.uuid4``,
``os.urandom`` and ``os.getrandom``, and the classes ``datetime.datetime`` and
``datetime.date`` (whose ``now``, ``utcnow`` and ``today`` are guarded). A module that bound one of
the originals before jarvis was imported (``from time import time``) keeps the original;
``scrub_module(module)`` rebinds such names, and the node applies it to strategy modules.

``PYTHONHASHSEED`` makes ``set`` and ``dict`` iteration orders over strings reproducible;
``ensure_hash_seed(seed)`` re-executes the interpreter with a value derived from the node seed
when it is not set.
"""

from __future__ import annotations

import contextlib
import datetime as _datetime
import functools
import os
import random as _random
import sys
import threading
import time as _time
import types
import uuid as _uuid
from collections.abc import Callable, Iterator
from typing import Any

__all__ = [
    "NondeterminismError",
    "active",
    "ensure_hash_seed",
    "guard",
    "hash_seed_for",
    "install",
    "scrub_module",
]


class NondeterminismError(RuntimeError):
    """A strategy callback called a nondeterministic function."""


_state = threading.local()
_installed = False
_originals: dict[int, Callable[..., Any]] = {}  # id(original) -> guarded replacement

_TIME_FUNCTIONS = (
    "time",
    "time_ns",
    "monotonic",
    "monotonic_ns",
    "perf_counter",
    "perf_counter_ns",
    "process_time",
    "process_time_ns",
    "localtime",
    "gmtime",
    "ctime",
    "strftime",
)
_RANDOM_FUNCTIONS = (
    "random",
    "uniform",
    "triangular",
    "randint",
    "choice",
    "randrange",
    "sample",
    "shuffle",
    "choices",
    "normalvariate",
    "lognormvariate",
    "expovariate",
    "vonmisesvariate",
    "gammavariate",
    "gauss",
    "betavariate",
    "paretovariate",
    "weibullvariate",
    "getrandbits",
    "randbytes",
    "binomialvariate",
    "seed",
)


def active() -> bool:
    """True while a guard is active on the current thread."""
    return getattr(_state, "depth", 0) > 0


def _check(name: str) -> None:
    if getattr(_state, "depth", 0) > 0:
        raise NondeterminismError(
            f"{name}() is not allowed in a strategy callback; use ctx.now() for time and "
            "ctx.rng(key) for randomness"
        )


def _guarded(name: str, original: Callable[..., Any]) -> Callable[..., Any]:
    @functools.wraps(original)
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        _check(name)
        return original(*args, **kwargs)

    wrapper.__jarvis_original__ = original  # type: ignore[attr-defined]
    _originals[id(original)] = wrapper
    return wrapper


def _patch(module: types.ModuleType, attribute: str, qualified: str) -> None:
    original = getattr(module, attribute, None)
    if original is None or hasattr(original, "__jarvis_original__"):
        return
    setattr(module, attribute, _guarded(qualified, original))


class _GuardedDateTime(_datetime.datetime):
    # No instance dict: the replacement keeps datetime's layout, which compiled extensions such
    # as pyarrow check when they import it.
    __slots__ = ()

    @classmethod
    def now(cls, tz: _datetime.tzinfo | None = None) -> _datetime.datetime:  # type: ignore[override]
        _check("datetime.now")
        return _ORIGINAL_DATETIME.now(tz)

    @classmethod
    def utcnow(cls) -> _datetime.datetime:  # type: ignore[override]
        _check("datetime.utcnow")
        return _ORIGINAL_DATETIME.utcnow()

    @classmethod
    def today(cls) -> _datetime.datetime:  # type: ignore[override]
        _check("datetime.today")
        return _ORIGINAL_DATETIME.today()


class _GuardedDate(_datetime.date):
    __slots__ = ()

    @classmethod
    def today(cls) -> _datetime.date:  # type: ignore[override]
        _check("date.today")
        return _ORIGINAL_DATE.today()


_ORIGINAL_DATETIME = _datetime.datetime
_ORIGINAL_DATE = _datetime.date


def install() -> None:
    """Replaces the nondeterministic functions with guarded versions (idempotent)."""
    global _installed
    if _installed:
        return
    for name in _TIME_FUNCTIONS:
        _patch(_time, name, f"time.{name}")
    for name in _RANDOM_FUNCTIONS:
        _patch(_random, name, f"random.{name}")
    _patch(_uuid, "uuid1", "uuid.uuid1")
    _patch(_uuid, "uuid4", "uuid.uuid4")
    _patch(os, "urandom", "os.urandom")
    _patch(os, "getrandom", "os.getrandom")
    _datetime.datetime = _GuardedDateTime  # type: ignore[misc]
    _datetime.date = _GuardedDate  # type: ignore[misc]
    _installed = True


def scrub_module(module: types.ModuleType) -> list[str]:
    """Rebinds names in `module` that refer to unguarded originals; returns the names."""
    install()
    rebound = []
    for name, value in list(vars(module).items()):
        replacement = _originals.get(id(value))
        if replacement is not None:
            setattr(module, name, replacement)
            rebound.append(name)
        elif value is _ORIGINAL_DATETIME:
            setattr(module, name, _GuardedDateTime)
            rebound.append(name)
        elif value is _ORIGINAL_DATE:
            setattr(module, name, _GuardedDate)
            rebound.append(name)
    return rebound


@contextlib.contextmanager
def guard() -> Iterator[None]:
    """Forbids nondeterministic calls on this thread for the duration of the block."""
    install()
    _state.depth = getattr(_state, "depth", 0) + 1
    try:
        yield
    finally:
        _state.depth -= 1


def hash_seed_for(seed: int) -> int:
    """PYTHONHASHSEED derived from a node seed (0 to 2**32 - 1)."""
    x = (seed + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return (x ^ (x >> 31)) & 0xFFFFFFFF


def ensure_hash_seed(seed: int) -> None:
    """Re-executes the interpreter with PYTHONHASHSEED set when it is missing.

    Returns normally when PYTHONHASHSEED is already set (whatever its value; the event log
    header records it). Otherwise it does not return.
    """
    if os.environ.get("PYTHONHASHSEED") is not None:
        return
    os.environ["PYTHONHASHSEED"] = str(hash_seed_for(seed))
    sys.stdout.flush()
    sys.stderr.flush()
    # orig_argv keeps interpreter options and "-c <code>", which sys.argv drops.
    os.execv(sys.executable, [sys.executable, *sys.orig_argv[1:]])
