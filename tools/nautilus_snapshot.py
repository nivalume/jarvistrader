#!/usr/bin/env python3
"""Extract the nautilus_trader model contract into a JSON snapshot.

    tools/nautilus_snapshot.py --nautilus /path/to/nautilus_trader > tests/conformance/nautilus_<commit>.json

The snapshot records what jarvis promises to stay compatible with (docs/architecture.md
section 6): enum variants with their integer values and strings, fixed-point constants of the
standard-precision build, the field names of the aligned structs, and the built-in currencies.
jarvis/model/enums.hpp and jarvis/model/currencies.hpp are generated from it by
tools/gen_model_tables.py, and tests/tools/test_nautilus_conformance.py checks the bindings
against it. The source tree must contain crates/model/src.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

STRUCTS = {
    # name: path relative to crates/model/src
    "QuoteTick": "data/quote.rs",
    "TradeTick": "data/trade.rs",
    "Bar": "data/bar.rs",
    "BarSpecification": "data/bar.rs",
    "BookOrder": "data/order.rs",
    "OrderBookDelta": "data/delta.rs",
    "OrderBookDeltas": "data/deltas.rs",
    "InstrumentStatus": "data/status.rs",
    "MarkPriceUpdate": "data/prices.rs",
    "IndexPriceUpdate": "data/prices.rs",
    "FundingRateUpdate": "data/funding.rs",
    "InstrumentClose": "data/close.rs",
    "CurrencyPair": "instruments/currency_pair.rs",
    "CryptoPerpetual": "instruments/crypto_perpetual.rs",
    "CryptoFuture": "instruments/crypto_future.rs",
    "Currency": "types/currency.rs",
    "AccountBalance": "types/balance.rs",
    "MarginBalance": "types/balance.rs",
    "AccountState": "events/account/state.rs",
    "OrderInitialized": "events/order/initialized.rs",
    "OrderDenied": "events/order/denied.rs",
    "OrderEmulated": "events/order/emulated.rs",
    "OrderReleased": "events/order/released.rs",
    "OrderSubmitted": "events/order/submitted.rs",
    "OrderAccepted": "events/order/accepted.rs",
    "OrderRejected": "events/order/rejected.rs",
    "OrderCanceled": "events/order/canceled.rs",
    "OrderExpired": "events/order/expired.rs",
    "OrderTriggered": "events/order/triggered.rs",
    "OrderPendingUpdate": "events/order/pending_update.rs",
    "OrderPendingCancel": "events/order/pending_cancel.rs",
    "OrderModifyRejected": "events/order/modify_rejected.rs",
    "OrderCancelRejected": "events/order/cancel_rejected.rs",
    "OrderUpdated": "events/order/updated.rs",
    "OrderFilled": "events/order/filled.rs",
    "OrderFillVoided": "events/order/fill_voided.rs",
    "PositionOpened": "events/position/opened.rs",
    "PositionChanged": "events/position/changed.rs",
    "PositionClosed": "events/position/closed.rs",
    "PositionAdjusted": "events/position/adjusted.rs",
}

LEGACY_NONE_TOKENS = {
    "OrderSide": "NO_ORDER_SIDE",
    "PositionSide": "NO_POSITION_SIDE",
    "ContingencyType": "NO_CONTINGENCY",
    "TrailingOffsetType": "NO_TRAILING_OFFSET",
    "TriggerType": "NO_TRIGGER",
}

CONSTANTS = {
    "FIXED_PRECISION": ("types/fixed.rs", "FIXED_PRECISION_STANDARD"),
    "PRICE_MAX": ("types/price.rs", "PRICE_MAX"),
    "PRICE_MIN": ("types/price.rs", "PRICE_MIN"),
    "QUANTITY_MAX": ("types/quantity.rs", "QUANTITY_MAX"),
    "QUANTITY_MIN": ("types/quantity.rs", "QUANTITY_MIN"),
    "MONEY_MAX": ("types/money.rs", "MONEY_MAX"),
    "MONEY_MIN": ("types/money.rs", "MONEY_MIN"),
}


def shouty_snake(name: str) -> str:
    """strum SCREAMING_SNAKE_CASE (heck): split words at lower/digit-to-upper boundaries."""
    words: list[str] = []
    for part in name.split("_"):
        current = ""
        for i, ch in enumerate(part):
            prev = part[i - 1] if i else ""
            nxt = part[i + 1] if i + 1 < len(part) else ""
            boundary = current and ch.isupper() and (
                prev.islower() or (prev.isupper() and nxt.islower())
            )
            if boundary:
                words.append(current)
                current = ""
            current += ch
        if current:
            words.append(current)
    return "_".join(w.upper() for w in words)


def enum_attributes(lines: list[str], index: int) -> str:
    """Attribute text above `pub enum` at `index`, back to the previous item or blank line."""
    collected = []
    for j in range(index - 1, max(-1, index - 60), -1):
        line = lines[j].strip()
        if not line or line == "}" or line.startswith(("pub ", "impl", "fn ", "use ", "mod ")):
            break
        collected.append(line)
    return "\n".join(reversed(collected))


def parse_enums(text: str) -> dict:
    enums: dict = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        match = re.match(r"pub enum (\w+) \{", lines[i].strip())
        if not match:
            i += 1
            continue
        name = match.group(1)
        case_insensitive = "ascii_case_insensitive" in enum_attributes(lines, i)
        variants = []
        pending: list[str] = []
        i += 1
        while not lines[i].strip().startswith("}"):
            body = lines[i].strip()
            if body.startswith("#["):
                pending.append(body)
            variant = re.match(r"(\w+)\s*=\s*([^,]+),", body)
            if variant:
                vname = variant.group(1)
                expr = variant.group(2).split("//")[0].strip()
                shift = re.fullmatch(r"(\d+)\s*<<\s*(\d+)", expr)
                value = int(shift.group(1)) << int(shift.group(2)) if shift else int(expr)
                display = shouty_snake(vname)
                aliases = []
                for attr in pending:
                    to_string = re.search(r'to_string\s*=\s*"([^"]+)"', attr)
                    serialize = re.findall(r'serialize\s*=\s*"([^"]+)"', attr)
                    if to_string:
                        display = to_string.group(1)
                    aliases += [a for a in serialize if a != display]
                variants.append({"name": vname, "value": value, "string": display, "aliases": aliases})
                pending = []
            i += 1
        if variants:
            enums[name] = {"case_insensitive": case_insensitive, "variants": variants}
        i += 1
    return enums


def parse_struct_fields(text: str, name: str) -> list[str]:
    match = re.search(r"pub struct " + name + r"\s*\{(.*?)\n\}", text, re.S)
    if not match:
        raise SystemExit(f"struct {name} not found")
    return re.findall(r"^\s*pub (\w+):", match.group(1), re.M)


def parse_constant(text: str, ident: str) -> float:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = re.match(rf"\s*pub const {ident}: \w+ = ([-0-9_.]+);", line)
        if not match:
            continue
        guard = " ".join(lines[max(0, index - 3):index])
        if 'feature = "high-precision"' in guard and "not(" not in guard:
            continue
        return float(match.group(1).replace("_", ""))
    raise SystemExit(f"constant {ident} not found")


def parse_currencies(text: str) -> list[dict]:
    pattern = re.compile(r'^\s+[A-Z0-9_]+ => "([^"]+)", (\d+), (\d+), "([^"]*)", (\w+);', re.M)
    return [
        {"code": m[0], "precision": int(m[1]), "iso4217": int(m[2]), "name": m[3], "type": m[4]}
        for m in pattern.findall(text)
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nautilus", type=Path, required=True)
    args = parser.parse_args()
    src = args.nautilus / "crates" / "model" / "src"
    commit = subprocess.run(
        ["git", "-C", str(args.nautilus), "rev-parse", "HEAD"], capture_output=True, text=True
    ).stdout.strip()

    enums_text = (src / "enums.rs").read_text()
    for enum, token in LEGACY_NONE_TOKENS.items():
        if token not in enums_text:
            raise SystemExit(f"legacy token {token} for {enum} not found")
    snapshot = {
        "source": {"repository": "nautechsystems/nautilus_trader", "commit": commit},
        "enums": parse_enums(enums_text),
        "legacy_none_tokens": LEGACY_NONE_TOKENS,
        "constants": {
            key: parse_constant((src / path).read_text(), ident)
            for key, (path, ident) in CONSTANTS.items()
        },
        "structs": {
            name: parse_struct_fields((src / path).read_text(), name) for name, path in STRUCTS.items()
        },
        "currencies": parse_currencies((src / "currencies.rs").read_text()),
    }
    json.dump(snapshot, sys.stdout, indent=1, ensure_ascii=False, sort_keys=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
