from __future__ import annotations

import sys
from pathlib import Path

from conftest import REPO_ROOT, load_tool

sys.path.insert(0, str(REPO_ROOT / "tools" / "tla"))
select_specs = load_tool("tla/select_specs.py")
mapfile = load_tool("tla/mapfile.py")

SPEC_MAP = mapfile.SpecMap(
    specs=(
        mapfile.Spec("OrderLifecycle", ("jarvis/execution/order_fsm.hpp", "jarvis/model/events/order_*.hpp"),
                     True, True, 10, ()),
        mapfile.Spec("DepthSync", ("jarvis/adapter/binance/depth_sync*.hpp",), True, False, 5, ()),
    ),
    always=("jarvis/core/**", "specs/tla/**"),
    core_extra=("jarvis/execution/**", "specs/**"),
)


def test_glob_semantics() -> None:
    assert mapfile.matches("jarvis/core/a.hpp", "jarvis/core/**")
    assert mapfile.matches("jarvis/core/x/y/a.hpp", "jarvis/core/**")
    assert mapfile.matches("jarvis/model/events/order_filled.hpp", "jarvis/model/events/order_*.hpp")
    assert not mapfile.matches("jarvis/model/events/sub/order_filled.hpp", "jarvis/model/events/order_*.hpp")
    assert mapfile.matches("a/b/c.txt", "**/c.txt")
    assert mapfile.matches("c.txt", "**/c.txt")
    assert not mapfile.matches("jarvis/corex/a.hpp", "jarvis/core/**")


def test_mapped_path_selects_its_spec_only() -> None:
    files = ["jarvis/execution/order_fsm.hpp", "README.md"]
    assert select_specs.affected_specs(files, SPEC_MAP) == ["OrderLifecycle"]


def test_global_path_selects_every_spec() -> None:
    assert select_specs.affected_specs(["jarvis/core/rng.hpp"], SPEC_MAP) == ["OrderLifecycle", "DepthSync"]


def test_editing_a_spec_file_selects_it() -> None:
    assert select_specs.affected_specs(["specs/tla/DepthSync.cfg"], SPEC_MAP) == ["OrderLifecycle", "DepthSync"]


def test_unrelated_change_selects_nothing_and_is_not_core() -> None:
    files = ["docs/architecture.md", "python/jarvis/__init__.py"]
    assert select_specs.affected_specs(files, SPEC_MAP) == []
    assert not select_specs.touches_core(files, SPEC_MAP)


def test_core_is_union_of_spec_paths_and_extra() -> None:
    assert select_specs.touches_core(["jarvis/adapter/binance/depth_sync.hpp"], SPEC_MAP)
    assert select_specs.touches_core(["jarvis/execution/oms.hpp"], SPEC_MAP)
    assert "jarvis/adapter/binance/depth_sync*.hpp" in SPEC_MAP.core_globs()


def test_repository_generated_files_are_current() -> None:
    assert select_specs.main(["--check"]) == 0


def test_repository_map_parses_and_specs_have_files(repo_root: Path) -> None:
    spec_map = mapfile.load()
    for spec in spec_map.specs:
        assert (repo_root / "specs" / "tla" / f"{spec.name}.tla").exists()
        assert (repo_root / "specs" / "tla" / f"{spec.name}.cfg").exists()
