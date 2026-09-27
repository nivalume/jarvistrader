"""Parse TLC's printed values and the behaviour files TLC writes in simulation mode.

TLC prints values in TLA+ syntax: strings, integers, TRUE/FALSE, model values (bare names),
tuples << >>, sets { }, records [f |-> v], and functions (k :> v @@ ...). `-simulate file=...`
writes each behaviour as a module of STATE_n definitions, each a conjunction of
`/\\ variable = value`. These parse into Python values:

    string -> str      integer -> int       boolean -> bool     model value -> ModelValue
    tuple  -> tuple    set -> frozenset     record, function -> dict
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, order=True)
class ModelValue:
    name: str

    def __str__(self) -> str:
        return self.name


Value = str | int | bool | ModelValue | tuple | frozenset | dict

_TOKEN = re.compile(
    r"""\s*(?:
        (?P<string>"(?:[^"\\]|\\.)*")
      | (?P<number>-?\d+)
      | (?P<punct><<|>>|\|->|:>|@@|/\\|==|[{}\[\](),=])
      | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
    )""",
    re.VERBOSE,
)


class ParseError(ValueError):
    pass


def _tokens(text: str) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    pos = 0
    while True:
        while pos < len(text) and text[pos].isspace():
            pos += 1
        if pos >= len(text):
            return out
        match = _TOKEN.match(text, pos)
        if not match or match.end() == pos:
            raise ParseError(f"unexpected text at {pos}: {text[pos:pos + 20]!r}")
        kind = match.lastgroup
        assert kind is not None
        out.append((kind, match.group(kind)))
        pos = match.end()


class _Parser:
    def __init__(self, text: str) -> None:
        self.toks = _tokens(text)
        self.i = 0

    def peek(self) -> tuple[str, str] | None:
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self, expected: str | None = None) -> tuple[str, str]:
        tok = self.peek()
        if tok is None:
            raise ParseError(f"unexpected end, expected {expected or 'a value'}")
        if expected is not None and tok[1] != expected:
            raise ParseError(f"expected {expected!r}, found {tok[1]!r}")
        self.i += 1
        return tok

    def at(self, text: str) -> bool:
        tok = self.peek()
        return tok is not None and tok[1] == text

    def done(self) -> bool:
        return self.i >= len(self.toks)

    def items(self, close: str) -> list[Value]:
        out: list[Value] = []
        if self.at(close):
            self.take(close)
            return out
        while True:
            out.append(self.value())
            if self.at(","):
                self.take(",")
                continue
            self.take(close)
            return out

    def value(self) -> Value:
        kind, text = self.take()
        if kind == "string":
            return re.sub(r"\\(.)", r"\1", text[1:-1])
        if kind == "number":
            return int(text)
        if kind == "name":
            if text == "TRUE":
                return True
            if text == "FALSE":
                return False
            return ModelValue(text)
        if text == "<<":
            return tuple(self.items(">>"))
        if text == "{":
            return frozenset(self.items("}"))
        if text == "[":
            record: dict[Value, Value] = {}
            if self.at("]"):
                self.take("]")
                return record
            while True:
                field = self.take()[1]
                self.take("|->")
                record[field] = self.value()
                if self.at(","):
                    self.take(",")
                    continue
                self.take("]")
                return record
        if text == "(":
            function: dict[Value, Value] = {}
            while True:
                key = self.value()
                self.take(":>")
                function[key] = self.value()
                if self.at("@@"):
                    self.take("@@")
                    continue
                self.take(")")
                return function
        raise ParseError(f"unexpected {text!r}")


def parse_value(text: str) -> Value:
    parser = _Parser(text)
    value = parser.value()
    if not parser.done():
        raise ParseError(f"trailing text after value: {parser.peek()}")
    return value


def parse_state(text: str) -> dict[str, Value]:
    """A conjunction `/\\ x = v /\\ y = w ...` (a single `x = v` is accepted too)."""
    parser = _Parser(text)
    state: dict[str, Value] = {}
    while not parser.done():
        if parser.at("/\\"):
            parser.take("/\\")
        kind, name = parser.take()
        if kind != "name":
            raise ParseError(f"expected a variable name, found {name!r}")
        parser.take("=")
        state[name] = parser.value()
    return state


_STATE_HEADER = re.compile(r"^STATE_(\d+) ==\s*$", re.MULTILINE)


def parse_behaviour_module(text: str) -> list[dict[str, Value]]:
    """The states of one behaviour file written by `tlc2.TLC -simulate file=...`."""
    body = text
    lines = body.splitlines()
    if lines and lines[0].startswith("----"):
        body = "\n".join(lines[1:])
    body = re.sub(r"^=+\s*$", "", body, flags=re.MULTILINE)
    parts = _STATE_HEADER.split(body)
    # parts: [preamble, n1, block1, n2, block2, ...]
    states: list[tuple[int, dict[str, Value]]] = []
    for k in range(1, len(parts), 2):
        states.append((int(parts[k]), parse_state(parts[k + 1])))
    states.sort(key=lambda item: item[0])
    numbers = [n for n, _ in states]
    if numbers != list(range(1, len(numbers) + 1)):
        raise ParseError(f"states are not numbered 1..n: {numbers}")
    return [state for _, state in states]


def read_behaviour_module(path: Path) -> list[dict[str, Value]]:
    return parse_behaviour_module(path.read_text(encoding="utf-8"))


def render(value: Value) -> str:
    """One whitespace-free token for the behaviour text format (tools/tla/behaviours.py)."""
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, ModelValue):
        return value.name
    if isinstance(value, str):
        if not value or re.search(r"[\s|=,{}:<>]", value):
            raise ValueError(f"string {value!r} cannot be rendered as a bare token")
        return value
    if isinstance(value, frozenset):
        return "{" + ",".join(sorted(render(v) for v in value)) + "}"
    if isinstance(value, dict):
        pairs = sorted(f"{render(k)}:{render(v)}" for k, v in value.items())
        return "{" + ",".join(pairs) + "}"
    if isinstance(value, tuple):
        if not value:
            return "{}"  # TLC prints the empty function as << >>
        return "<" + ",".join(render(v) for v in value) + ">"
    raise TypeError(f"cannot render {value!r}")
