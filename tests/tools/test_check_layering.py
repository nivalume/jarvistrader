from __future__ import annotations

from pathlib import Path

from conftest import load_tool

layering = load_tool("check-layering.py")


def write(root: Path, relative: str, content: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)


def messages(root: Path) -> list[str]:
    return [v.message for v in layering.check_tree(root)]


def test_repository_is_clean(repo_root: Path) -> None:
    assert layering.check_tree(repo_root) == []


def test_lower_and_same_layer_includes_are_allowed(tmp_path: Path) -> None:
    write(tmp_path, "jarvis/risk/gate.hpp",
          '#include "jarvis/core/status.hpp"\n#include "jarvis/execution/oms.hpp"\n'
          '#include "jarvis/risk/rule.hpp"\n#include <cstdint>\n')
    assert messages(tmp_path) == []


def test_upward_include_is_rejected(tmp_path: Path) -> None:
    write(tmp_path, "jarvis/model/price.hpp", '#include "jarvis/engine/engine.hpp"\n')
    assert any("higher layer 'engine'" in m for m in messages(tmp_path))


def test_siblings_of_equal_rank_are_rejected(tmp_path: Path) -> None:
    write(tmp_path, "jarvis/data/router.hpp", '#include "jarvis/cost/fee_model.hpp"\n')
    assert any("sibling" in m for m in messages(tmp_path))


def test_kernel_may_not_include_shell_or_forbidden_headers(tmp_path: Path) -> None:
    write(tmp_path, "jarvis/core/a.hpp",
          '#include "jarvis/live/node.hpp"\n#include <chrono>\n#include <iostream>\n'
          '#include <asio.hpp>\n#include <simdjson.h>\n')
    found = messages(tmp_path)
    assert any("shell layer 'live'" in m for m in found)
    assert any("<chrono>" in m for m in found)
    assert any("<iostream>" in m for m in found)
    assert any("asio.hpp" in m for m in found)
    assert any("simdjson.h" in m for m in found)


def test_kernel_is_header_only(tmp_path: Path) -> None:
    write(tmp_path, "jarvis/core/impl.cpp", "int x = 0;\n")
    assert any("header-only" in m for m in messages(tmp_path))


def test_shell_rules(tmp_path: Path) -> None:
    write(tmp_path, "jarvis/network/ws.cpp", '#include "jarvis/core/status.hpp"\n#include <asio.hpp>\n')
    write(tmp_path, "jarvis/node/config.cpp", '#include "jarvis/engine/engine.hpp"\n')
    assert messages(tmp_path) == []
    write(tmp_path, "jarvis/network/bad.cpp", '#include "jarvis/model/price.hpp"\n')
    write(tmp_path, "jarvis/node/bad.cpp", '#include "jarvis/adapter/binance/client.hpp"\n#include <asio.hpp>\n')
    found = messages(tmp_path)
    assert any("'network' must not include 'model'" in m for m in found)
    assert any("'node' must not include 'adapter'" in m for m in found)
    assert any("only network, adapter and live may include asio.hpp" in m for m in found)


def test_only_python_may_include_nanobind(tmp_path: Path) -> None:
    write(tmp_path, "python/src/bindings.cpp", "#include <nanobind/nanobind.h>\n")
    assert messages(tmp_path) == []
    write(tmp_path, "jarvis/live/x.cpp", "#include <nanobind/nanobind.h>\n")
    assert any("only python/src may include" in m for m in messages(tmp_path))


def test_examples_use_public_api_only(tmp_path: Path) -> None:
    write(tmp_path, "examples/cpp/ok.cpp",
          '#include "jarvis/strategy/strategy.hpp"\n#include "jarvis/node/node_main.hpp"\n')
    assert messages(tmp_path) == []
    write(tmp_path, "examples/cpp/bad.cpp", '#include "jarvis/backtest/replay.hpp"\n')
    assert any("public API" in m for m in messages(tmp_path))


def test_unknown_layer_directory_is_reported(tmp_path: Path) -> None:
    (tmp_path / "jarvis" / "misc").mkdir(parents=True)
    assert "unknown layer directory" in messages(tmp_path)
