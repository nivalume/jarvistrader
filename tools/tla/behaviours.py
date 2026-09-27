#!/usr/bin/env python3
"""Generate behaviours of a spec for forward trace validation (docs/architecture.md 18.2).

    behaviours.py --spec OrderLifecycle [--num 200] [--depth 16] [--seed 1] [--out FILE]
    behaviours.py --check tests/trace/behaviours/OrderLifecycle.txt

TLC runs specs/tla/<Spec>Behaviours.tla in simulation mode and writes every behaviour it
generates; this script turns them into a text file that tests/trace/trace_driver.cpp reads:

    spec <Spec> num <N> depth <D> seed <S>
    const <Name> <value>              one per CONSTANT in <Spec>Behaviours.cfg
    behaviour <k>
    step <Action> <args...> | <var>=<value> ...
    ...
    end

The first step of a behaviour is `Init`; each later step names the action taken and the state
it reached. Values are single tokens: integers, TRUE/FALSE, strings and model values bare,
sets and functions as {a,b} and {k:v}. A spec's `trace_vars` in MAP.toml limits the variables
written to the ones its trace driver compares. `--check` regenerates a committed file from the
parameters on its `spec` line and fails when the result differs.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mapfile  # noqa: E402
import run_tlc  # noqa: E402
import tla_values  # noqa: E402

SPEC_DIR = mapfile.REPO_ROOT / "specs" / "tla"
DEFAULT_OUT = mapfile.REPO_ROOT / "build" / "tla" / "behaviours"
HEADER = re.compile(r"^spec (\w+) num (\d+) depth (\d+) seed (-?\d+)$", re.MULTILINE)
_KEYWORDS = {
    "CONSTANT", "CONSTANTS", "SPECIFICATION", "INIT", "NEXT", "INVARIANT", "INVARIANTS",
    "PROPERTY", "PROPERTIES", "CHECK_DEADLOCK", "SYMMETRY", "VIEW", "CONSTRAINT", "CONSTRAINTS",
    "ACTION_CONSTRAINT", "ACTION_CONSTRAINTS",
}


def constants(cfg: Path) -> list[tuple[str, tla_values.Value]]:
    """`Name = value` assignments of the CONSTANTS section of a TLC config."""
    out: list[tuple[str, tla_values.Value]] = []
    section = False
    for raw in cfg.read_text(encoding="utf-8").splitlines():
        line = raw.split("\\*", 1)[0].strip()
        if not line:
            continue
        head = line.split()[0]
        if head in _KEYWORDS:
            section = head in {"CONSTANT", "CONSTANTS"}
            line = line[len(head):].strip()
            if not line:
                continue
        if section and "=" in line:
            name, value = line.split("=", 1)
            out.append((name.strip(), tla_values.parse_value(value.strip())))
    return out


def run_simulation(spec: str, num: int, depth: int, seed: int, work: Path) -> list[Path]:
    module = f"{spec}Behaviours"
    if not (SPEC_DIR / f"{module}.tla").exists():
        raise SystemExit(f"behaviours: {module}.tla does not exist in specs/tla")
    jar = run_tlc.ensure_jar()
    dump = work / "dump"
    dump.mkdir()
    argv = [
        "java", "-XX:+UseParallelGC", "-cp", str(jar), "tlc2.TLC",
        "-simulate", f"file={dump / 'b'},num={num}", "-depth", str(depth), "-seed", str(seed),
        "-workers", "1", "-metadir", str(work / "meta"), "-config", f"{module}.cfg",
        f"{module}.tla",
    ]
    result = subprocess.run(argv, cwd=SPEC_DIR, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-40:])
        raise SystemExit(f"behaviours: TLC failed on {module} (exit {result.returncode}):\n{tail}")

    def order(path: Path) -> tuple[int, ...]:
        return tuple(int(x) for x in path.name.split("_")[1:])

    files = sorted(dump.iterdir(), key=order)
    if len(files) != num:
        raise SystemExit(f"behaviours: TLC wrote {len(files)} behaviours, expected {num}")
    return files


def action_tokens(action: tla_values.Value) -> list[str]:
    if not isinstance(action, tuple) or not action or not isinstance(action[0], str):
        raise SystemExit(f"behaviours: malformed action {action!r}")
    return [action[0], *(tla_values.render(a) for a in action[1:])]


def render_file(spec: str, num: int, depth: int, seed: int, work: Path) -> str:
    lines = [
        f"# Behaviours of {spec} (specs/tla/{spec}Behaviours.tla), written by",
        "# tools/tla/behaviours.py; regenerate with its --check/--out options, never by hand.",
        f"spec {spec} num {num} depth {depth} seed {seed}",
    ]
    for name, value in constants(SPEC_DIR / f"{spec}Behaviours.cfg"):
        lines.append(f"const {name} {tla_values.render(value)}")
    keep = set(mapfile.load().spec(spec).trace_vars)
    for k, path in enumerate(run_simulation(spec, num, depth, seed, work), start=1):
        lines.append(f"behaviour {k}")
        for state in tla_values.read_behaviour_module(path):
            action = state.pop("action", None)
            names = sorted(n for n in state if not keep or n in keep)
            fields = " ".join(f"{n}={tla_values.render(state[n])}" for n in names)
            lines.append(f"step {' '.join(action_tokens(action))} | {fields}")
        lines.append("end")
    return "\n".join(lines) + "\n"


def generate(spec: str, num: int, depth: int, seed: int) -> str:
    with tempfile.TemporaryDirectory(prefix="jarvis-behaviours-") as tmp:
        return render_file(spec, num, depth, seed, Path(tmp))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate spec behaviours with TLC")
    parser.add_argument("--spec")
    parser.add_argument("--num", type=int, default=200)
    parser.add_argument("--depth", type=int, default=16)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", type=Path, help="regenerate a committed file and compare")
    args = parser.parse_args(argv)

    if args.check:
        current = args.check.read_text(encoding="utf-8")
        match = HEADER.search(current)
        if not match:
            raise SystemExit(f"behaviours: {args.check} has no `spec ... seed ...` line")
        spec, num, depth, seed = match.group(1), *(int(g) for g in match.groups()[1:])
        fresh = generate(spec, num, depth, seed)
        if fresh != current:
            print(f"behaviours: {args.check} is stale; regenerate with\n"
                  f"  python3 tools/tla/behaviours.py --spec {spec} --num {num} "
                  f"--depth {depth} --seed {seed} --out {args.check}")
            return 1
        print(f"behaviours: {args.check} is up to date")
        return 0

    if not args.spec:
        parser.error("--spec or --check is required")
    mapfile.load().spec(args.spec)  # fails on unknown specs
    text = generate(args.spec, args.num, args.depth, args.seed)
    out = args.out or DEFAULT_OUT / f"{args.spec}.txt"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    count = text.count("\nbehaviour ")
    steps = text.count("\nstep ") - count
    print(f"behaviours: {args.spec}: {count} behaviours, {steps} steps -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
