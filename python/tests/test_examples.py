"""The trade logger examples: the Python and C++ versions write the same run log.

Needs the C++ binaries (golden_node, trade_logger) of a CMake build; set JARVIS_BIN_DIR to their
directory, or build the dev preset (build/dev/bin). Skipped when they are missing.
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


def _example():
    spec = importlib.util.spec_from_file_location("trade_logger", ROOT / "examples/py/trade_logger.py")
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
