"""Shared helpers for specs/tla/MAP.toml: loading and glob matching."""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MAP_PATH = REPO_ROOT / "specs" / "tla" / "MAP.toml"


@dataclass(frozen=True)
class Spec:
    name: str
    paths: tuple[str, ...]
    forward: bool
    backward: bool
    budget_min: int
    tlc_args: tuple[str, ...]
    trace_vars: tuple[str, ...] = ()  # the variables behaviour files keep (all when empty)


@dataclass(frozen=True)
class SpecMap:
    specs: tuple[Spec, ...]
    always: tuple[str, ...]
    core_extra: tuple[str, ...]

    def spec(self, name: str) -> Spec:
        for spec in self.specs:
            if spec.name == name:
                return spec
        raise KeyError(f"unknown spec {name!r}; known: {', '.join(s.name for s in self.specs)}")

    def core_globs(self) -> list[str]:
        globs: list[str] = []
        for pattern in [*self.core_extra, *(p for s in self.specs for p in s.paths)]:
            if pattern not in globs:
                globs.append(pattern)
        return sorted(globs)


def load(path: Path = MAP_PATH) -> SpecMap:
    with path.open("rb") as handle:
        raw = tomllib.load(handle)
    specs = []
    for name, entry in sorted(raw.get("specs", {}).items()):
        specs.append(
            Spec(
                name=name,
                paths=tuple(entry.get("paths", [])),
                forward=bool(entry.get("forward", False)),
                backward=bool(entry.get("backward", False)),
                budget_min=int(entry.get("budget_min", 10)),
                tlc_args=tuple(entry.get("tlc_args", [])),
                trace_vars=tuple(entry.get("trace_vars", [])),
            )
        )
    return SpecMap(
        specs=tuple(specs),
        always=tuple(raw.get("global", {}).get("always", [])),
        core_extra=tuple(raw.get("core", {}).get("extra", [])),
    )


@lru_cache(maxsize=None)
def _compile(pattern: str) -> re.Pattern[str]:
    """Translate a path glob: ** spans directories, * and ? stay within one path segment."""
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def matches(path: str, pattern: str) -> bool:
    return _compile(pattern).match(path) is not None


def matches_any(path: str, patterns) -> bool:
    return any(matches(path, p) for p in patterns)
