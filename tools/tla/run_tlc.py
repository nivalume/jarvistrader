#!/usr/bin/env python3
"""Run TLC on specs listed in specs/tla/MAP.toml (docs/architecture.md section 17.4).

    run_tlc.py --spec SeqOrder         # one spec
    run_tlc.py --all                   # every spec
    run_tlc.py --changed --base main   # specs affected by the change (see select_specs.py)

The pinned tla2tools.jar (specs/tla/tla2tools.lock) is downloaded to build/tools/ on first use
and its SHA-256 is verified every time.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mapfile  # noqa: E402
import select_specs as spec_select  # noqa: E402

SPEC_DIR = mapfile.REPO_ROOT / "specs" / "tla"
LOCK_FILE = SPEC_DIR / "tla2tools.lock"
TOOLS_DIR = mapfile.REPO_ROOT / "build" / "tools"


def ensure_jar() -> Path:
    with LOCK_FILE.open("rb") as handle:
        lock = tomllib.load(handle)
    jar = TOOLS_DIR / f"tla2tools-{lock['version']}.jar"
    if not jar.exists():
        TOOLS_DIR.mkdir(parents=True, exist_ok=True)
        partial = jar.with_suffix(".part")
        subprocess.run(["curl", "-fsSL", "-o", str(partial), lock["url"]], check=True)
        partial.rename(jar)
    digest = hashlib.sha256(jar.read_bytes()).hexdigest()
    if digest != lock["sha256"]:
        jar.unlink()
        raise SystemExit(f"{jar.name}: sha256 {digest} does not match lock {lock['sha256']}")
    return jar


def run_spec(jar: Path, spec: mapfile.Spec) -> bool:
    meta = mapfile.REPO_ROOT / "build" / "tla" / spec.name
    if meta.exists():
        shutil.rmtree(meta)
    meta.mkdir(parents=True)
    argv = [
        "java", "-XX:+UseParallelGC", "-cp", str(jar), "tlc2.TLC",
        "-workers", "auto", "-metadir", str(meta), "-config", f"{spec.name}.cfg",
        *spec.tlc_args, f"{spec.name}.tla",
    ]
    print(f"tlc: {spec.name}: {' '.join(argv[4:])}", flush=True)
    try:
        result = subprocess.run(
            argv, cwd=SPEC_DIR, capture_output=True, text=True, timeout=spec.budget_min * 60
        )
    except subprocess.TimeoutExpired:
        print(f"tlc: {spec.name}: FAIL: exceeded the {spec.budget_min}-minute budget")
        return False
    tail = [line for line in result.stdout.splitlines() if line.strip()][-8:]
    ok = result.returncode == 0 and "No error has been found" in result.stdout
    for line in tail if not ok else tail[-3:]:
        print(f"    {line}")
    print(f"tlc: {spec.name}: {'ok' if ok else f'FAIL (exit {result.returncode})'}")
    return ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run TLC on jarvis specs")
    parser.add_argument("--spec", action="append", default=[])
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--changed", action="store_true")
    parser.add_argument("--base", default=None)
    parser.add_argument("--worktree", action="store_true")
    args = parser.parse_args(argv)

    spec_map = mapfile.load()
    if args.all:
        names = [s.name for s in spec_map.specs]
    elif args.changed:
        files = spec_select.changed_files(args.base, args.worktree)
        names = spec_select.affected_specs(files, spec_map)
    else:
        names = args.spec
    if not names:
        print("tlc: no specs selected")
        return 0

    jar = ensure_jar()
    failures = [n for n in names if not run_spec(jar, spec_map.spec(n))]
    print(f"tlc: {len(names)} spec(s), {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
