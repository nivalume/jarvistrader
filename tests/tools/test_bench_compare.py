from __future__ import annotations

import json
from pathlib import Path

from conftest import load_tool

bench_compare = load_tool("bench_compare.py")

THRESHOLDS = """
[defaults]
max_regression_pct = 10.0
abs_floor_ns = 1.0
gating = true
min_reproductions = 2

[groups.report]
gating = false
"""


def write_run(path: Path, group: str, times: dict[str, list[float]], unit: str = "ns") -> Path:
    runs = []
    for name, values in times.items():
        for index, value in enumerate(values):
            runs.append({"name": name, "run_name": name, "run_type": "iteration",
                         "repetition_index": index, "cpu_time": value, "real_time": value,
                         "time_unit": unit})
        runs.append({"name": f"{name}_median", "run_name": name, "run_type": "aggregate",
                     "aggregate_name": "median", "cpu_time": 0.0, "real_time": 0.0,
                     "time_unit": unit})
    path.write_text(json.dumps({"context": {"executable": f"/x/bench_{group}"}, "benchmarks": runs}))
    return path


def run(tmp_path: Path, rounds: list[tuple[float, float]], group: str = "hot",
        name: str = "a/b") -> int:
    thresholds = tmp_path / "t.toml"
    thresholds.write_text(THRESHOLDS)
    files: list[str] = []
    for index, (base, head) in enumerate(rounds):
        files.append(str(write_run(tmp_path / f"b{index}.json", group, {name: [base] * 5})))
        files.append(str(write_run(tmp_path / f"h{index}.json", group, {name: [head] * 5})))
    return bench_compare.main(["--thresholds", str(thresholds), *files])


def test_no_change_passes(tmp_path: Path) -> None:
    assert run(tmp_path, [(100.0, 101.0)] * 3) == 0


def test_regression_in_two_of_three_rounds_fails(tmp_path: Path) -> None:
    assert run(tmp_path, [(100.0, 115.0), (100.0, 102.0), (100.0, 116.0)]) == 1


def test_regression_in_one_of_three_rounds_passes(tmp_path: Path) -> None:
    assert run(tmp_path, [(100.0, 130.0), (100.0, 101.0), (100.0, 99.0)]) == 0


def test_single_round_needs_that_round_to_regress(tmp_path: Path) -> None:
    assert run(tmp_path, [(100.0, 120.0)]) == 1
    assert run(tmp_path, [(100.0, 105.0)]) == 0


def test_absolute_floor_ignores_tiny_changes(tmp_path: Path) -> None:
    assert run(tmp_path, [(2.0, 2.8)] * 3) == 0  # +40% but only 0.8 ns


def test_report_group_never_gates(tmp_path: Path) -> None:
    assert run(tmp_path, [(100.0, 200.0)] * 3, group="report") == 0


def test_median_ignores_one_outlier(tmp_path: Path) -> None:
    thresholds = tmp_path / "t.toml"
    thresholds.write_text(THRESHOLDS)
    base = write_run(tmp_path / "b.json", "hot", {"x": [100.0, 100.0, 100.0, 100.0, 100.0]})
    head = write_run(tmp_path / "h.json", "hot", {"x": [100.0, 100.0, 100.0, 100.0, 900.0]})
    assert bench_compare.main(["--thresholds", str(thresholds), str(base), str(head)]) == 0


def test_units_are_normalized(tmp_path: Path) -> None:
    group, medians = bench_compare.load_results(
        write_run(tmp_path / "u.json", "hot", {"x": [1.5]}, unit="us"), "cpu_time")
    assert group == "hot"
    assert medians["x"] == 1500.0


def test_threshold_resolution_order() -> None:
    config = {"defaults": {"max_regression_pct": 10.0}, "groups": {"hot": {"max_regression_pct": 5.0}},
              "benchmarks": {"x": {"max_regression_pct": 3.0}}}
    assert bench_compare.policy_for(config, "hot", "x").max_regression_pct == 3.0
    assert bench_compare.policy_for(config, "hot", "y").max_regression_pct == 5.0
    assert bench_compare.policy_for(config, "report", "y").max_regression_pct == 10.0
