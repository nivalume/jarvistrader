"""Tools for trace validation: the TLC value parser, behaviours.py and check_trace.py."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from conftest import REPO_ROOT, load_tool

sys.path.insert(0, str(REPO_ROOT / "tools" / "tla"))
tla_values = load_tool("tla/tla_values.py")
behaviours = load_tool("tla/behaviours.py")
check_trace = load_tool("tla/check_trace.py")
select_specs = load_tool("tla/select_specs.py")

MV = tla_values.ModelValue


def test_values_of_every_kind() -> None:
    assert tla_values.parse_value('"a\\"b"') == 'a"b'
    assert tla_values.parse_value("-12") == -12
    assert tla_values.parse_value("TRUE") is True
    assert tla_values.parse_value("t1") == MV("t1")
    assert tla_values.parse_value('<<"Fill", t1, 2>>') == ("Fill", MV("t1"), 2)
    assert tla_values.parse_value("<< >>") == ()
    assert tla_values.parse_value('{ "A",\n  "B" }') == frozenset({"A", "B"})
    assert tla_values.parse_value("[a |-> 1, b |-> {}]") == {"a": 1, "b": frozenset()}
    assert tla_values.parse_value("(t1 :> 2 @@ t2 :> 0)") == {MV("t1"): 2, MV("t2"): 0}
    with pytest.raises(tla_values.ParseError):
        tla_values.parse_value("<<1, 2")
    with pytest.raises(tla_values.ParseError):
        tla_values.parse_value("1 2")


def test_render_is_one_token_per_value() -> None:
    assert tla_values.render(frozenset({"B", "A"})) == "{A,B}"
    assert tla_values.render({MV("t2"): 1, MV("t1"): 3}) == "{t1:3,t2:1}"
    assert tla_values.render(()) == "{}"
    assert tla_values.render(False) == "FALSE"
    with pytest.raises(ValueError):
        tla_values.render("two words")


DUMP = """---------------- MODULE /tmp/x/b_0_0 -----------------
STATE_2 ==
/\\ action = <<"Plain", "SUBMITTED">>
/\\ plain = { "DENIED",
  "SUBMITTED" }
/\\ status = "SUBMITTED"

STATE_1 ==
/\\ action = <<"Init">>
/\\ plain = {}
/\\ status = "INITIALIZED"

=================================================
"""


def test_behaviour_module_states_in_order() -> None:
    states = tla_values.parse_behaviour_module(DUMP)
    assert [s["status"] for s in states] == ["INITIALIZED", "SUBMITTED"]
    assert states[1]["plain"] == frozenset({"DENIED", "SUBMITTED"})
    assert behaviours.action_tokens(states[1]["action"]) == ["Plain", "SUBMITTED"]


def test_constants_of_a_config(tmp_path: Path) -> None:
    cfg = tmp_path / "X.cfg"
    cfg.write_text("CONSTANTS\n  MaxQty = 3 \\* note\n  TradeIds = {t1, t2}\n"
                   "SPECIFICATION Spec\nINVARIANT TypeOK\n")
    mv = behaviours.tla_values.ModelValue  # the module behaviours.py imported
    assert behaviours.constants(cfg) == [("MaxQty", 3), ("TradeIds", frozenset({mv("t1"), mv("t2")}))]


def test_committed_behaviour_files_name_their_parameters() -> None:
    for spec in ("OrderLifecycle", "TradingState", "Matching"):
        text = (REPO_ROOT / "tests" / "trace" / "behaviours" / f"{spec}.txt").read_text()
        match = behaviours.HEADER.search(text)
        assert match and match.group(1) == spec
        assert (REPO_ROOT / "specs" / "tla" / f"{spec}Behaviours.tla").exists()


TLC_DEADLOCK = """Error: Deadlock reached.
Error: The behavior up to this point is:
State 1: <Initial predicate>
/\\ o = 1
/\\ k = 0
/\\ status = "INITIALIZED"

State 2: <Replay line 20, col 1 to line 25, col 10 of module OrderLifecycleTrace>
/\\ o = 1
/\\ k = 1
/\\ status = "SUBMITTED"
/\\ prev = "INITIALIZED"
/\\ quantity = 1
/\\ filled = 0

15 states generated, 2 distinct states found, 0 states left on queue.
"""

TRACE = """Orders == <<
  [id |-> "O-1", quantity |-> 1, steps |-> <<
      [seq |-> "8", refused |-> FALSE, a |-> <<"Plain", "SUBMITTED">>, status |-> "SUBMITTED", prev |-> "INITIALIZED", quantity |-> 1, filled |-> 0],
      [seq |-> "9", refused |-> TRUE, a |-> <<"Plain", "ACCEPTED">>, status |-> "SUBMITTED", prev |-> "INITIALIZED", quantity |-> 1, filled |-> 0]>>]
>>
"""


def test_a_deadlock_names_the_order_and_the_step() -> None:
    orders = check_trace.orders_of(TRACE)
    assert orders[0]["id"] == "O-1"
    text = check_trace.explain(TLC_DEADLOCK, orders)
    assert "order O-1" in text
    assert "seq 9 Plain ACCEPTED" in text
    assert "refused" in text


def test_selection_reports_forward_and_backward_specs(tmp_path: Path, monkeypatch) -> None:
    out = tmp_path / "github_output"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    assert select_specs.main(["--files", "jarvis/risk/rate_limit.hpp", "--github-output"]) == 0
    lines = dict(line.split("=", 1) for line in out.read_text().splitlines())
    assert lines["specs"] == "TradingState"
    assert lines["forward"] == "TradingState"
    assert lines["backward"] == ""
