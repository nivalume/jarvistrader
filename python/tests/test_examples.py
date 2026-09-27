"""The examples: the Python and C++ versions of each write the same run log.

Needs the C++ binaries (golden_node, trade_logger, pegged_mm) of a CMake build; set
JARVIS_BIN_DIR to their directory, or build the dev preset (build/dev/bin). Skipped when they
are missing.
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
from pathlib import Path

import pytest

from jarvis import log

ROOT = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get("JARVIS_BIN_DIR", ROOT / "build" / "dev" / "bin"))
CONFIG = ROOT / "examples" / "config" / "trade_logger.toml"


def _example(name: str = "trade_logger"):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"examples/py/{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.skipif(not (BIN / "trade_logger").exists(), reason="C++ examples not built")
def test_python_and_cpp_trade_loggers_write_the_same_log(tmp_path: Path) -> None:
    catalog = tmp_path / "catalog"
    subprocess.run(
        [BIN / "golden_node", "catalog", "--seed", "7", "--out", catalog], check=True
    )
    sets = [
        "--set", f"data.catalog={catalog}",
        "--set", "data.range.start=2026-09-01T00:00:00Z",
        "--set", "data.range.end=2026-09-01T00:10:00Z",
        "--set", 'data.streams.0.streams=["aggTrade", "bookTicker"]',
        "--set", "strategies.tlog-001.params.sample_ms=20000",
    ]
    subprocess.run(
        [BIN / "trade_logger", "--config", CONFIG, *sets, "--out", tmp_path / "cpp"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    example = _example()
    code = example.jarvis.node.run_main(
        [example.TradeLogger], ["--config", str(CONFIG), *sets, "--out", str(tmp_path / "py")]
    )
    assert code == 0
    equal, compared, first_diff, detail = log.compare(str(tmp_path / "cpp"), str(tmp_path / "py"), "all")
    assert equal, (first_diff, detail)
    assert compared > 1000
    assert sum(1 for r in log.read(str(tmp_path / "py")) if r.is_output) > 40


@pytest.mark.skipif(not (BIN / "pegged_mm").exists(), reason="C++ examples not built")
def test_python_and_cpp_market_makers_write_the_same_log(tmp_path: Path) -> None:
    catalog = tmp_path / "catalog"
    subprocess.run(
        [BIN / "golden_node", "catalog", "--seed", "7", "--out", catalog, "--instrument"],
        check=True,
    )
    config = ROOT / "examples" / "config" / "mm_quote.toml"
    sets = [
        "--set", f"data.catalog={catalog}",
        "--set", "data.range.start=2026-09-01T00:00:00Z",
        "--set", "data.range.end=2026-09-01T00:05:00Z",
    ]
    subprocess.run(
        [BIN / "pegged_mm", "--config", config, *sets, "--out", tmp_path / "cpp"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    example = _example("mm_quote")
    code = example.jarvis.node.run_main(
        [example.MmQuote], ["--config", str(config), *sets, "--out", str(tmp_path / "py")]
    )
    assert code == 0
    equal, compared, first_diff, detail = log.compare(str(tmp_path / "cpp"), str(tmp_path / "py"), "all")
    assert equal, (first_diff, detail)
    report = example.jarvis.RunReport.from_run(tmp_path / "py")
    row = report.row("mm-001", "BTCUSDT-PERP.BINANCE")
    assert row is not None and row.fills > 20
    assert row.taker_fills == 0  # post-only quotes never take
    assert report.orders["mm-001"].submitted > 20
