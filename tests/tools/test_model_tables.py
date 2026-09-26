from __future__ import annotations

import json

from conftest import REPO_ROOT, load_tool

gen_model_tables = load_tool("gen_model_tables.py")
nautilus_snapshot = load_tool("nautilus_snapshot.py")


def test_generated_tables_are_current() -> None:
    assert gen_model_tables.main(["--check"]) == 0


def test_snapshot_pins_the_aligned_commit() -> None:
    snapshot = json.loads((REPO_ROOT / "tests/conformance/nautilus_cd417b80.json").read_text())
    assert snapshot["source"]["commit"].startswith("cd417b80")
    assert snapshot["constants"]["FIXED_PRECISION"] == 9
    assert [v["value"] for v in snapshot["enums"]["OrderStatus"]["variants"]] == list(range(1, 16))


def test_shouty_snake_matches_strum() -> None:
    cases = {
        "PartiallyFilled": "PARTIALLY_FILLED",
        "L1_MBP": "L1_MBP",
        "FX": "FX",
        "Cfd": "CFD",
        "NoLiquiditySide": "NO_LIQUIDITY_SIDE",
        "DoubleBidAsk": "DOUBLE_BID_ASK",
        "AtTheOpen": "AT_THE_OPEN",
    }
    for name, expected in cases.items():
        assert nautilus_snapshot.shouty_snake(name) == expected
