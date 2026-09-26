"""The bindings against the nautilus_trader contract snapshot (docs/architecture.md section 6).

tests/conformance/nautilus_cd417b80.json is extracted from nautilus_trader by
tools/nautilus_snapshot.py. Every aligned struct field must exist as an attribute, except the
documented omissions below (section 6.8), and enum values, strings and the currency table must
match exactly.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from jarvis import model as m

SNAPSHOT = json.loads(
    (Path(__file__).resolve().parents[2] / "tests" / "conformance" / "nautilus_cd417b80.json").read_text()
)

# Fields jarvis leaves out on purpose (architecture section 6.8): free-form maps and variable
# lists that do not fit fixed-size kernel records.
OMISSIONS = {
    "CurrencyPair": {"tick_scheme", "info"},
    "CryptoPerpetual": {"tick_scheme", "info"},
    "CryptoFuture": {"tick_scheme", "info"},
    "AccountState": {"info"},
    "OrderInitialized": {"linked_order_ids", "exec_algorithm_params"},
    "OrderFilled": {"info"},
    "OrderFillVoided": {"info"},
}

# Python class names that differ from the nautilus Rust struct names.
CLASS_NAMES = {"OrderBookDepth": "OrderBookDepth10"}


@pytest.mark.parametrize("struct", sorted(SNAPSHOT["structs"]))
def test_struct_fields_are_bound(struct: str) -> None:
    cls = getattr(m, CLASS_NAMES.get(struct, struct), None)
    assert cls is not None, f"jarvis.model has no class {struct}"
    expected = set(SNAPSHOT["structs"][struct]) - OMISSIONS.get(struct, set())
    missing = sorted(name for name in expected if not hasattr(cls, name))
    assert not missing, f"{struct} lacks {missing}"


def test_omissions_are_real() -> None:
    for struct, names in OMISSIONS.items():
        assert names <= set(SNAPSHOT["structs"][struct]), f"stale omission for {struct}"
        cls = getattr(m, struct)
        assert not any(hasattr(cls, name) for name in names), f"{struct} now binds an omission"


@pytest.mark.parametrize("enum", sorted(SNAPSHOT["enums"]))
def test_enum_values_and_strings(enum: str) -> None:
    cls = getattr(m, enum)
    variants = SNAPSHOT["enums"][enum]["variants"]
    assert [(member.name, member.value) for member in cls] == [(v["string"], v["value"]) for v in variants]
    for v in variants:
        for text in [v["string"], v["string"].lower(), *v["aliases"]]:
            assert cls.from_str(text).value == v["value"]


def test_currency_table() -> None:
    type_strings = {v["name"]: v["string"] for v in SNAPSHOT["enums"]["CurrencyType"]["variants"]}
    for c in SNAPSHOT["currencies"]:
        cur = m.Currency.from_str(c["code"])
        assert (cur.code, cur.precision, cur.iso4217, cur.name, cur.currency_type.name) == (
            c["code"],
            c["precision"],
            c["iso4217"],
            c["name"],
            type_strings[c["type"]],
        )
