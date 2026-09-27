#!/usr/bin/env python3
"""Backward trace validation (docs/architecture.md 18.2): check an exported trace with TLC.

    check_trace.py --spec OrderLifecycle --dir DIR                 # DIR/<Spec>Trace.tla + .cfg
    check_trace.py --spec OrderLifecycle --log RUN_DIR --jarvis BIN # export with BIN, then check
    check_trace.py --spec OrderLifecycle --golden                  # every golden case's trace

`jarvis trace-export` writes the trace module (jarvis/node/trace_export.hpp). TLC runs it with
the spec next to it; a deadlock is a step the spec does not allow, and the report names the
order, the log sequence number and the action.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mapfile  # noqa: E402
import run_tlc  # noqa: E402
import tla_values  # noqa: E402

SPEC_DIR = mapfile.REPO_ROOT / "specs" / "tla"
GOLDEN_DIR = mapfile.REPO_ROOT / "tests" / "golden"


def orders_of(module_text: str) -> list[dict]:
    """The Orders constant of a trace module."""
    match = re.search(r"^Orders == (<<.*?^>>)", module_text, re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit("check_trace: the trace module has no Orders definition")
    value = tla_values.parse_value(match.group(1))
    assert isinstance(value, tuple)
    return list(value)


def last_state(output: str) -> dict[str, tla_values.Value] | None:
    blocks = re.split(r"^State \d+: .*$", output, flags=re.MULTILINE)
    if len(blocks) < 2:
        return None
    text = blocks[-1]
    end = re.search(r"^\s*$\n^(?!/\\)", text, re.MULTILINE)
    body = text[: end.start()] if end else text
    lines = [line for line in body.splitlines() if line.strip()]
    conjunction: list[str] = []
    for line in lines:
        if line.startswith("/\\") or conjunction:
            if not line.startswith("/\\") and not line.startswith(" "):
                break
            conjunction.append(line)
    return tla_values.parse_state("\n".join(conjunction))


def explain(output: str, orders: list[dict]) -> str:
    state = last_state(output)
    if not state or "o" not in state or "k" not in state:
        return ""
    o, k = int(state["o"]), int(state["k"])  # type: ignore[arg-type]
    order = orders[o - 1]
    steps = order["steps"]
    lines = [f"  order {order['id']} (quantity {order['quantity']} units), after {k} step(s):"]
    lines.append(f"    state: status={state.get('status')} prev={state.get('prev')} "
                 f"quantity={state.get('quantity')} filled={state.get('filled')}")
    if k < len(steps):
        step = steps[k]
        action = " ".join(tla_values.render(a) for a in step["a"])
        after = (f"status={step['status']} prev={step['prev']} quantity={step['quantity']} "
                 f"filled={step['filled']}")
        if step["refused"]:
            what = "the implementation refused it; the spec enables it, or its state differs"
        else:
            what = "the spec does not allow it, or reaches a state other than the implementation's"
        lines.append(f"    next: seq {step['seq']} {action}: {what}")
        lines.append(f"    implementation after it: {after}")
    return "\n".join(lines)


def check_dir(spec: str, directory: Path, label: str) -> bool:
    module = f"{spec}Trace"
    tla = directory / f"{module}.tla"
    cfg = directory / f"{module}.cfg"
    return check_files(spec, tla, cfg, label)


def check_files(spec: str, tla: Path, cfg: Path, label: str) -> bool:
    module = f"{spec}Trace"
    jar = run_tlc.ensure_jar()
    with tempfile.TemporaryDirectory(prefix="jarvis-trace-") as tmp:
        work = Path(tmp)
        shutil.copy(SPEC_DIR / f"{spec}.tla", work / f"{spec}.tla")
        shutil.copy(tla, work / f"{module}.tla")
        shutil.copy(cfg, work / f"{module}.cfg")
        argv = [
            "java", "-XX:+UseParallelGC", "-cp", str(jar), "tlc2.TLC", "-workers", "1",
            "-metadir", str(work / "meta"), "-config", f"{module}.cfg", f"{module}.tla",
        ]
        result = subprocess.run(argv, cwd=work, capture_output=True, text=True, timeout=1800)
    text = tla.read_text(encoding="utf-8")
    orders = orders_of(text)
    steps = sum(len(o["steps"]) for o in orders)
    if result.returncode == 0 and "No error has been found" in result.stdout:
        print(f"check_trace: {label}: ok ({len(orders)} orders, {steps} steps)")
        return True
    reason = "a step the spec does not allow" if "Deadlock reached" in result.stdout else \
        next((line for line in result.stdout.splitlines() if line.startswith("Error:")), "TLC failed")
    print(f"check_trace: {label}: FAIL: {reason}")
    detail = explain(result.stdout, orders)
    if detail:
        print(detail)
    else:
        print("\n".join(result.stdout.splitlines()[-30:]))
    return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check an exported trace against its spec")
    parser.add_argument("--spec", default="OrderLifecycle")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--dir", type=Path, help="directory with <Spec>Trace.tla and .cfg")
    source.add_argument("--log", type=Path, help="event log or run directory to export first")
    source.add_argument("--golden", action="store_true", help="the traces of the golden cases")
    parser.add_argument("--jarvis", type=Path, help="the jarvis CLI (with --log)")
    args = parser.parse_args(argv)

    if args.dir:
        return 0 if check_dir(args.spec, args.dir, str(args.dir)) else 1
    if args.log:
        if not args.jarvis:
            parser.error("--log needs --jarvis")
        with tempfile.TemporaryDirectory(prefix="jarvis-export-") as tmp:
            subprocess.run([str(args.jarvis), "trace-export", str(args.log), "--spec", args.spec,
                            "--out", tmp], check=True)
            return 0 if check_dir(args.spec, Path(tmp), str(args.log)) else 1
    cases = sorted(GOLDEN_DIR.glob(f"*/expected.{args.spec}Trace.tla"))
    if not cases:
        print(f"check_trace: no golden case has an expected.{args.spec}Trace.tla")
        return 1
    ok = True
    for tla in cases:
        cfg = tla.with_suffix(".cfg")
        ok = check_files(args.spec, tla, cfg, tla.parent.name) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
