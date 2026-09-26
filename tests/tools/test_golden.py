from __future__ import annotations

import textwrap
from pathlib import Path

from conftest import load_tool

golden = load_tool("golden.py")

WRITER = "import sys, pathlib; pathlib.Path(sys.argv[1]).write_bytes(sys.argv[2].encode())"


def make_case(root: Path, name: str, content: str) -> Path:
    case = root / name
    case.mkdir(parents=True)
    (case / "case.toml").write_text(
        textwrap.dedent(
            f"""
            command = ["{{python}}", "-c", "{WRITER}", "{{out}}/result.txt", "{content}"]

            [[compare]]
            produced = "result.txt"
            expected = "expected.result.txt"
            """
        )
    )
    return case


def test_no_cases_passes(tmp_path: Path) -> None:
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 0


def test_update_then_check_round_trip(tmp_path: Path) -> None:
    case = make_case(tmp_path, "hello", "hello")
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 1  # no expectation
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path), "--update"]) == 0
    assert (case / "expected.result.txt").read_text() == "hello"
    assert "expected.result.txt" in (case / "expected.sha256").read_text()
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 0


def test_changed_output_fails(tmp_path: Path) -> None:
    case = make_case(tmp_path, "drift", "one")
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path), "--update"]) == 0
    toml = (case / "case.toml").read_text().replace('"one"', '"two"')
    (case / "case.toml").write_text(toml)
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 1


def test_hand_edited_expectation_is_caught(tmp_path: Path, capsys) -> None:
    case = make_case(tmp_path, "edited", "abc")
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path), "--update"]) == 0
    (case / "expected.result.txt").write_text("abc")  # same bytes: digest still matches
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 0
    (case / "expected.result.txt").write_text("abd")
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 1
    assert "does not match its recorded digest" in capsys.readouterr().out


def test_unknown_case_name_is_an_error(tmp_path: Path) -> None:
    make_case(tmp_path, "present", "x")
    assert golden.main(["--root", str(tmp_path), "--case", "absent"]) == 2


def test_commands_run_in_order(tmp_path: Path) -> None:
    case = tmp_path / "steps"
    case.mkdir()
    copy = "import sys, shutil; shutil.copyfile(sys.argv[1], sys.argv[2])"
    (case / "case.toml").write_text(
        textwrap.dedent(
            f"""
            commands = [
              ["{{python}}", "-c", "{WRITER}", "{{out}}/first.txt", "step"],
              ["{{python}}", "-c", "{copy}", "{{out}}/first.txt", "{{out}}/second.txt"],
            ]

            [[compare]]
            produced = "second.txt"
            expected = "expected.second.txt"
            """
        )
    )
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path), "--update"]) == 0
    assert (case / "expected.second.txt").read_text() == "step"
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 0


def test_failing_step_stops_the_case(tmp_path: Path, capsys) -> None:
    case = tmp_path / "broken"
    case.mkdir()
    (case / "case.toml").write_text(
        textwrap.dedent(
            f"""
            commands = [
              ["{{python}}", "-c", "import sys; sys.exit(3)"],
              ["{{python}}", "-c", "{WRITER}", "{{out}}/result.txt", "never"],
            ]

            [[compare]]
            produced = "result.txt"
            expected = "expected.result.txt"
            """
        )
    )
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path), "--update"]) == 1
    assert "command exited with 3" in capsys.readouterr().out
    assert not (case / "expected.result.txt").exists()


def test_command_and_commands_are_exclusive(tmp_path: Path) -> None:
    case = tmp_path / "both"
    case.mkdir()
    (case / "case.toml").write_text(
        'command = ["a"]\ncommands = [["b"]]\n[[compare]]\nproduced = "x"\nexpected = "y"\n'
    )
    assert golden.main(["--root", str(tmp_path), "--bin", str(tmp_path)]) == 2
