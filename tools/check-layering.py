#!/usr/bin/env python3
"""Check include-based layering (docs/architecture.md section 3).

Rules:
  * Kernel layers are ordered core < model < {data, cost} < portfolio < execution < risk
    < strategy < engine < backtest. A kernel header may include its own layer or a lower one,
    never a sibling of the same rank and never a shell layer.
  * Kernel layers are header-only and must not include threading, clock, stream, random,
    exception or locale headers, nor any third-party library.
  * Shell layers: network may include core; node may include every kernel layer; adapter may
    include network and every kernel layer; live may include everything under jarvis/.
    Only network, adapter and live may include the network stack; only python/src may include
    nanobind or Python.h.
  * examples/ may include only the public API: core, model, data, strategy and node.
  * Every directory directly under jarvis/ must be a known layer.

The freestanding build target catches `throw` in kernel headers; this script catches the
dependency direction, which a compiler cannot.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

KERNEL_RANK = {
    "core": 0,
    "model": 1,
    "data": 2,
    "cost": 2,
    "portfolio": 3,
    "execution": 4,
    "risk": 5,
    "strategy": 6,
    "engine": 7,
    "backtest": 8,
}
SHELL_ALLOWED = {
    "network": {"core"},
    "node": set(KERNEL_RANK),
    "adapter": set(KERNEL_RANK) | {"network"},
    "live": set(KERNEL_RANK) | {"network", "adapter", "node", "live"},
}
EXAMPLES_ALLOWED = {"core", "model", "data", "strategy", "node"}

KERNEL_FORBIDDEN_STD = {
    "iostream", "istream", "ostream", "fstream", "sstream", "iomanip", "random", "thread",
    "mutex", "shared_mutex", "condition_variable", "future", "atomic", "semaphore", "latch",
    "barrier", "stop_token", "chrono", "ctime", "time.h", "exception", "stdexcept", "typeinfo",
    "regex", "locale", "filesystem", "cstdio", "stdio.h",
}
NETWORK_STACK_PREFIXES = ("asio", "openssl/", "picohttpparser", "boost/")
PYTHON_PREFIXES = ("nanobind/", "Python.h", "pybind11/")
THIRD_PARTY_PREFIXES = NETWORK_STACK_PREFIXES + PYTHON_PREFIXES + (
    "simdjson", "toml++/", "doctest/", "benchmark/", "sbe/",
)
NETWORK_LAYERS = {"network", "adapter", "live"}
SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".cxx", ".ipp", ".inl"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


def layer_of_include(header: str) -> str | None:
    parts = header.split("/")
    if len(parts) >= 3 and parts[0] == "jarvis":
        return parts[1]
    return None


def check_include(owner: str, header: str) -> str | None:
    """Return a message if a file in layer `owner` may not include `header`."""
    target = layer_of_include(header)
    is_kernel = owner in KERNEL_RANK

    if header.startswith(PYTHON_PREFIXES) and owner != "python":
        return f"only python/src may include {header}"
    if header.startswith(NETWORK_STACK_PREFIXES) and owner not in NETWORK_LAYERS | {"python"}:
        return f"only network, adapter and live may include {header}"

    if is_kernel:
        if header in KERNEL_FORBIDDEN_STD:
            return f"kernel layer '{owner}' must not include <{header}>"
        if header.startswith(THIRD_PARTY_PREFIXES):
            return f"kernel layer '{owner}' must not include third-party header {header}"
        if target is None:
            return None
        if target not in KERNEL_RANK:
            return f"kernel layer '{owner}' must not include shell layer '{target}' ({header})"
        if target == owner or KERNEL_RANK[target] < KERNEL_RANK[owner]:
            return None
        if KERNEL_RANK[target] == KERNEL_RANK[owner]:
            return f"sibling kernel layers '{owner}' and '{target}' must not include each other"
        return f"'{owner}' (rank {KERNEL_RANK[owner]}) must not include higher layer '{target}' ({header})"

    if owner in SHELL_ALLOWED:
        if target is None or target == owner or target in SHELL_ALLOWED[owner]:
            return None
        return f"shell layer '{owner}' must not include '{target}' ({header})"

    if owner == "examples":
        if target is None or target in EXAMPLES_ALLOWED:
            return None
        return f"examples may include only the public API, not '{target}' ({header})"

    return None  # python/src: unrestricted


def owner_of(relative: Path) -> str | None:
    parts = relative.parts
    if parts[0] == "jarvis" and len(parts) >= 3:
        return parts[1]
    if parts[0] == "examples":
        return "examples"
    if parts[:2] == ("python", "src"):
        return "python"
    return None


def check_tree(root: Path) -> list[Violation]:
    violations: list[Violation] = []
    known = set(KERNEL_RANK) | set(SHELL_ALLOWED)
    jarvis_dir = root / "jarvis"
    if jarvis_dir.is_dir():
        for child in sorted(jarvis_dir.iterdir()):
            if child.is_dir() and child.name not in known:
                violations.append(Violation(f"jarvis/{child.name}", 0, "unknown layer directory"))

    roots = [root / "jarvis", root / "examples", root / "python" / "src"]
    files = sorted(p for base in roots if base.is_dir() for p in base.rglob("*") if p.is_file())
    for path in files:
        relative = path.relative_to(root)
        owner = owner_of(relative)
        if owner is None:
            continue
        rel = relative.as_posix()
        if owner in KERNEL_RANK and path.suffix in {".cpp", ".cc", ".cxx"}:
            violations.append(Violation(rel, 0, "kernel layers are header-only; move this to a shell layer"))
        if path.suffix not in SOURCE_SUFFIXES and not path.name.endswith(".hpp.in"):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if match is None:
                continue
            message = check_include(owner, match.group(2))
            if message:
                violations.append(Violation(rel, number, message))
    return violations


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check jarvis include layering")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)
    violations = check_tree(args.root)
    for violation in violations:
        print(violation)
    if violations:
        print(f"check-layering: {len(violations)} violation(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
