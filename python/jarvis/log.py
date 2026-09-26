"""Event logs (docs/architecture.md sections 5.6 and 16).

A log is a directory of segments ``events-NNNNNN.jlog``; each record carries the order key
``(ts, source_id, seq)`` and one event. ``fingerprint`` hashes the record bytes (not the header),
so two builds or two runs can be compared byte for byte.
"""

from __future__ import annotations

from collections.abc import Iterator

from ._core import log as _native

EventLogReader = _native.EventLogReader
EventLogWriter = _native.EventLogWriter
Record = _native.Record
compare = _native.compare
event_text = _native.event_text
fingerprint = _native.fingerprint
write_corpus = _native.write_corpus


def read(directory: str) -> Iterator[Record]:
    """Yields every record of the log in `directory`, in log order."""
    yield from EventLogReader(directory)


__all__ = [
    "EventLogReader",
    "EventLogWriter",
    "Record",
    "compare",
    "event_text",
    "fingerprint",
    "read",
    "write_corpus",
]
