#!/usr/bin/env python3
"""Golden-trace runner (docs/architecture.md section 17.2).

Every directory under the golden root that contains a ``case.toml`` is one case:

    command = ["{bin}/bin/jarvis", "replay", "{case}/input.jlog", "--out", "{out}/commands.jlog"]

    [[compare]]
    produced = "commands.jlog"             # relative to the scratch output directory {out}
    expected = "expected.commands.jlog"    # relative to the case directory

A case that needs several steps lists them as ``commands = [[...], [...]]`` instead of
``command``; they run in order and the first failure fails the case.

Placeholders: {bin} build directory, {case} case directory, {out} scratch output directory,
{root} repository root, {python} the running interpreter. ``expected.sha256`` records the
digest of every expected file and is verified first, so a hand-edited expectation is caught.

Outputs are compared byte for byte. ``--update`` rewrites the expected files and their digests
so the change shows up in review.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import shutil
import subprocess
import sys
import tempfile
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DIGEST_FILE = "expected.sha256"


@dataclass(frozen=True)
class Compare:
    produced: str
    expected: str


@dataclass(frozen=True)
class Case:
    name: str
    directory: Path
    commands: list[list[str]]
    compares: list[Compare]


class CaseError(Exception):
    pass


def load_case(directory: Path, root: Path) -> Case:
    config_path = directory / "case.toml"
    with config_path.open("rb") as handle:
        config = tomllib.load(handle)
    if ("command" in config) == ("commands" in config):
        raise CaseError(f"{config_path}: give exactly one of 'command' and 'commands'")
    commands = [config["command"]] if "command" in config else config["commands"]
    if not isinstance(commands, list) or not commands:
        raise CaseError(f"{config_path}: 'commands' must be a non-empty list of commands")
    for command in commands:
        if (
            not isinstance(command, list)
            or not command
            or not all(isinstance(a, str) for a in command)
        ):
            raise CaseError(f"{config_path}: each command must be a non-empty list of strings")
    compares_raw = config.get("compare", [])
    if not isinstance(compares_raw, list) or not compares_raw:
        raise CaseError(f"{config_path}: at least one [[compare]] table is required")
    compares = []
    for item in compares_raw:
        if not isinstance(item, dict) or set(item) != {"produced", "expected"}:
            raise CaseError(f"{config_path}: [[compare]] needs exactly 'produced' and 'expected'")
        compares.append(Compare(produced=item["produced"], expected=item["expected"]))
    return Case(
        name=directory.relative_to(root).as_posix(),
        directory=directory,
        commands=commands,
        compares=compares,
    )


def discover(root: Path) -> list[Case]:
    if not root.is_dir():
        return []
    return [load_case(p.parent, root) for p in sorted(root.rglob("case.toml"))]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_digests(case: Case) -> dict[str, str]:
    path = case.directory / DIGEST_FILE
    digests: dict[str, str] = {}
    if not path.exists():
        return digests
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        digest, name = line.split(maxsplit=1)
        digests[name.strip()] = digest
    return digests


def write_digests(case: Case) -> None:
    lines = [f"{sha256(case.directory / c.expected)}  {c.expected}" for c in case.compares]
    (case.directory / DIGEST_FILE).write_text("\n".join(lines) + "\n")


def run_commands(case: Case, bin_dir: Path, out_dir: Path) -> subprocess.CompletedProcess[bytes]:
    """Runs the case's commands in order; returns the first failing result, else the last."""
    substitutions = {
        "bin": str(bin_dir),
        "case": str(case.directory),
        "out": str(out_dir),
        "root": str(REPO_ROOT),
        "python": sys.executable,
    }
    result: subprocess.CompletedProcess[bytes] | None = None
    for command in case.commands:
        argv = [arg.format(**substitutions) for arg in command]
        result = subprocess.run(argv, cwd=case.directory, capture_output=True, check=False)
        if result.returncode != 0:
            break
    assert result is not None
    return result


def describe_difference(produced: bytes, expected: bytes) -> str:
    try:
        produced_text = produced.decode("utf-8")
        expected_text = expected.decode("utf-8")
    except UnicodeDecodeError:
        offset = next(
            (i for i, (a, b) in enumerate(zip(produced, expected, strict=False)) if a != b),
            min(len(produced), len(expected)),
        )
        return (
            f"    first differing byte at offset {offset}; "
            f"produced {len(produced)} bytes, expected {len(expected)} bytes"
        )
    diff = difflib.unified_diff(
        expected_text.splitlines(), produced_text.splitlines(), "expected", "produced", lineterm=""
    )
    return "\n".join("    " + line for line in list(diff)[:40])


def check_case(case: Case, bin_dir: Path, update: bool) -> list[str]:
    problems: list[str] = []
    if not update:
        digests = read_digests(case)
        for compare in case.compares:
            expected_path = case.directory / compare.expected
            if not expected_path.exists():
                problems.append(f"  missing expected file {compare.expected}; run with --update")
                continue
            recorded = digests.get(compare.expected)
            if recorded is None:
                problems.append(f"  {DIGEST_FILE} has no entry for {compare.expected}")
            elif recorded != sha256(expected_path):
                problems.append(f"  {compare.expected} does not match its recorded digest")
        if problems:
            return problems

    with tempfile.TemporaryDirectory(prefix="golden-") as scratch:
        out_dir = Path(scratch)
        result = run_commands(case, bin_dir, out_dir)
        if result.returncode != 0:
            stderr = result.stderr.decode("utf-8", errors="replace").strip()
            return [f"  command exited with {result.returncode}", f"    {stderr[-2000:]}"]
        for compare in case.compares:
            produced_path = out_dir / compare.produced
            expected_path = case.directory / compare.expected
            if not produced_path.exists():
                problems.append(f"  command did not produce {compare.produced}")
                continue
            if update:
                expected_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(produced_path, expected_path)
                continue
            produced = produced_path.read_bytes()
            expected = expected_path.read_bytes()
            if produced != expected:
                problems.append(f"  {compare.produced} differs from {compare.expected}")
                problems.append(describe_difference(produced, expected))
        if update and not problems:
            write_digests(case)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT / "tests" / "golden")
    parser.add_argument("--bin", type=Path, default=REPO_ROOT / "build" / "dev")
    parser.add_argument("--update", action="store_true", help="rewrite expected outputs")
    parser.add_argument("--case", action="append", default=[], help="run only the named case")
    args = parser.parse_args(argv)

    try:
        cases = discover(args.root)
    except CaseError as error:
        print(f"golden: {error}", file=sys.stderr)
        return 2
    if args.case:
        wanted = set(args.case)
        cases = [c for c in cases if c.name in wanted]
        missing = wanted - {c.name for c in cases}
        if missing:
            print(f"golden: unknown case(s): {', '.join(sorted(missing))}", file=sys.stderr)
            return 2

    failed = 0
    for case in cases:
        problems = check_case(case, args.bin.resolve(), args.update)
        status = "updated" if args.update and not problems else ("ok" if not problems else "FAIL")
        print(f"golden: {case.name}: {status}")
        for line in problems:
            print(line)
        failed += bool(problems)
    print(f"golden: {len(cases)} case(s), {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
