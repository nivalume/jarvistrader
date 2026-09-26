#!/usr/bin/env python3
"""Compare google/benchmark results A/B (docs/architecture.md section 17.3).

Usage:
    bench_compare.py --thresholds benchmarks/thresholds.toml BASE.json HEAD.json [BASE HEAD ...]

Arguments come in (base, head) pairs; each pair is one A/B round measured on the same machine.
Each benchmark is summarized by the median over its repetitions. A round regresses when the
median grows by more than ``max_regression_pct`` and by more than ``abs_floor_ns``. A benchmark
is reported as a regression when it regresses in at least ``min_reproductions`` rounds (or in
every round when fewer rounds were run). Gating regressions make the exit status 1.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

UNIT_TO_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


@dataclass(frozen=True)
class Policy:
    max_regression_pct: float
    abs_floor_ns: float
    gating: bool
    min_reproductions: int


@dataclass
class Result:
    name: str
    group: str
    base_ns: list[float] = field(default_factory=list)
    head_ns: list[float] = field(default_factory=list)
    regressed_rounds: int = 0
    improved_rounds: int = 0


def load_results(path: Path, metric: str) -> tuple[str, dict[str, float]]:
    """Return (group, {benchmark name: median time in ns}) for one benchmark JSON file."""
    data = json.loads(path.read_text())
    executable = Path(data.get("context", {}).get("executable", "bench_unknown")).name
    group = executable.removeprefix("bench_")
    samples: dict[str, list[float]] = {}
    for run in data.get("benchmarks", []):
        if run.get("run_type", "iteration") != "iteration" or run.get("error_occurred"):
            continue
        name = run.get("run_name", run["name"])
        scale = UNIT_TO_NS[run.get("time_unit", "ns")]
        samples.setdefault(name, []).append(float(run[metric]) * scale)
    return group, {name: statistics.median(values) for name, values in samples.items()}


def load_thresholds(path: Path) -> dict:
    with path.open("rb") as handle:
        return tomllib.load(handle)


def policy_for(config: dict, group: str, name: str) -> Policy:
    merged = dict(config.get("defaults", {}))
    merged.update(config.get("groups", {}).get(group, {}))
    merged.update(config.get("benchmarks", {}).get(name, {}))
    return Policy(
        max_regression_pct=float(merged.get("max_regression_pct", 10.0)),
        abs_floor_ns=float(merged.get("abs_floor_ns", 0.0)),
        gating=bool(merged.get("gating", True)),
        min_reproductions=int(merged.get("min_reproductions", 2)),
    )


def compare(pairs: list[tuple[Path, Path]], config: dict, metric: str):
    results: dict[str, Result] = {}
    only_base: set[str] = set()
    only_head: set[str] = set()
    for base_path, head_path in pairs:
        base_group, base = load_results(base_path, metric)
        head_group, head = load_results(head_path, metric)
        if base_group != head_group:
            raise SystemExit(f"group mismatch: {base_path} is {base_group}, {head_path} is {head_group}")
        only_base |= {f"{base_group}:{n}" for n in base.keys() - head.keys()}
        only_head |= {f"{head_group}:{n}" for n in head.keys() - base.keys()}
        for name in sorted(base.keys() & head.keys()):
            key = f"{base_group}:{name}"
            result = results.setdefault(key, Result(name=name, group=base_group))
            policy = policy_for(config, base_group, name)
            result.base_ns.append(base[name])
            result.head_ns.append(head[name])
            delta = head[name] - base[name]
            pct = 100.0 * delta / base[name] if base[name] > 0 else 0.0
            if abs(delta) > policy.abs_floor_ns:
                if pct > policy.max_regression_pct:
                    result.regressed_rounds += 1
                elif pct < -policy.max_regression_pct:
                    result.improved_rounds += 1
    return results, sorted(only_base), sorted(only_head)


def verdict(result: Result, policy: Policy) -> str:
    rounds = len(result.base_ns)
    needed = min(policy.min_reproductions, rounds)
    if result.regressed_rounds >= needed:
        return "REGRESSION" if policy.gating else "regression (report only)"
    if result.improved_rounds >= needed:
        return "improved"
    return "ok"


def render(results: dict[str, Result], config: dict, only_base: list[str], only_head: list[str]):
    lines = [
        "| benchmark | group | base median | head median | change | regressed rounds | verdict |",
        "| --- | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    failures = 0
    for key in sorted(results):
        result = results[key]
        policy = policy_for(config, result.group, result.name)
        base = statistics.median(result.base_ns)
        head = statistics.median(result.head_ns)
        change = 100.0 * (head - base) / base if base > 0 else 0.0
        outcome = verdict(result, policy)
        failures += outcome == "REGRESSION"
        lines.append(
            f"| {result.name} | {result.group} | {base:.2f} ns | {head:.2f} ns | {change:+.1f}% "
            f"| {result.regressed_rounds}/{len(result.base_ns)} | {outcome} |"
        )
    for key in only_base:
        lines.append(f"| {key} | | | | | | removed in head |")
    for key in only_head:
        lines.append(f"| {key} | | | | | | new in head |")
    return "\n".join(lines), failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="A/B benchmark comparison")
    parser.add_argument("files", nargs="+", type=Path, help="BASE.json HEAD.json pairs")
    parser.add_argument("--thresholds", type=Path, required=True)
    parser.add_argument("--metric", choices=["cpu_time", "real_time"], default="cpu_time")
    parser.add_argument("--summary", type=Path, help="also append the markdown table here")
    args = parser.parse_args(argv)
    if len(args.files) % 2 != 0:
        parser.error("files must come in BASE HEAD pairs")
    pairs = list(zip(args.files[0::2], args.files[1::2], strict=True))

    config = load_thresholds(args.thresholds)
    results, only_base, only_head = compare(pairs, config, args.metric)
    table, failures = render(results, config, only_base, only_head)
    header = f"Benchmark A/B: {len(pairs)} pair(s), metric {args.metric}\n\n"
    print(header + table)
    if args.summary:
        with args.summary.open("a") as handle:
            handle.write(header + table + "\n")
    if failures:
        print(f"\n{failures} gating regression(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
