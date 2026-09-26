"""The determinism guard (docs/architecture.md section 7.7)."""

from __future__ import annotations

import datetime
import os
import random
import subprocess
import sys
import time
import types
import uuid

import pytest

from jarvis import determinism

FORBIDDEN = {
    "time.time": lambda: time.time(),
    "time.monotonic": lambda: time.monotonic(),
    "time.perf_counter_ns": lambda: time.perf_counter_ns(),
    "random.random": lambda: random.random(),
    "random.choice": lambda: random.choice([1, 2]),
    "datetime.now": lambda: datetime.datetime.now(),
    "datetime.utcnow": lambda: datetime.datetime.utcnow(),
    "date.today": lambda: datetime.date.today(),
    "uuid.uuid4": lambda: uuid.uuid4(),
    "os.urandom": lambda: os.urandom(8),
}


@pytest.mark.parametrize("name", sorted(FORBIDDEN))
def test_forbidden_inside_guard_allowed_outside(name: str) -> None:
    FORBIDDEN[name]()  # fine outside
    with determinism.guard():
        assert determinism.active()
        with pytest.raises(determinism.NondeterminismError, match=name.split(".")[-1]):
            FORBIDDEN[name]()
    assert not determinism.active()
    FORBIDDEN[name]()


def test_guards_nest_and_unwind_on_error() -> None:
    with pytest.raises(ZeroDivisionError):
        with determinism.guard():
            with determinism.guard():
                pass
            assert determinism.active()
            _ = 1 / 0
    assert not determinism.active()


def test_scrub_module_rebinds_names_bound_before_install() -> None:
    module = types.ModuleType("strategy")
    original_time = time.time.__jarvis_original__  # what `from time import time` saw earlier
    module.now = original_time
    module.clock = datetime.datetime.__mro__[1]  # the stdlib datetime class
    assert sorted(determinism.scrub_module(module)) == ["clock", "now"]
    with determinism.guard(), pytest.raises(determinism.NondeterminismError):
        module.now()
    with determinism.guard(), pytest.raises(determinism.NondeterminismError):
        module.clock.now()


def test_hash_seed_is_derived_and_re_exec_applies_it() -> None:
    assert determinism.hash_seed_for(42) == determinism.hash_seed_for(42)
    assert 0 <= determinism.hash_seed_for(42) < 2**32
    code = (
        "import os, sys; from jarvis import determinism;"
        "determinism.ensure_hash_seed(42);"
        "print(os.environ['PYTHONHASHSEED'], hash('jarvis'))"
    )
    env = {k: v for k, v in os.environ.items() if k != "PYTHONHASHSEED"}
    runs = {subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True, check=True).stdout for _ in range(2)}
    assert len(runs) == 1
    seed, _ = runs.pop().split()
    assert int(seed) == determinism.hash_seed_for(42)
