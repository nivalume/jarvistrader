"""Event logs from Python: every record kind survives a read and re-write byte for byte."""

from __future__ import annotations

from pathlib import Path

import pytest

from jarvis import log
from jarvis import model as m


def test_python_round_trip_reproduces_the_log(tmp_path: Path) -> None:
    source = tmp_path / "corpus"
    log.write_corpus(str(source), 7, 3000)
    records = list(log.read(str(source)))
    assert len(records) == 3000
    assert len({r.kind for r in records}) == 33  # every record kind

    copy = tmp_path / "copy"
    with log.EventLogWriter(str(copy), seed=7) as writer:
        for r in records:
            writer.append(r.event, seq=r.seq, ts=r.ts, source_id=r.source_id)
    assert writer.records == 3000
    assert log.fingerprint(str(copy)) == log.fingerprint(str(source))
    equal, compared, _, _ = log.compare(str(source), str(copy), "all")
    assert equal and compared == 3000


def test_header_records_the_build_and_python(tmp_path: Path) -> None:
    directory = tmp_path / "h"
    with log.EventLogWriter(str(directory), seed=99, config_hash=bytes(range(32))):
        pass
    header = log.EventLogReader(str(directory)).header
    assert header["seed"] == 99
    assert header["config_hash"] == bytes(range(32))
    assert header["python_version"].count(".") == 2
    assert header["platform"]


def test_written_events_read_back_equal(tmp_path: Path) -> None:
    iid = m.InstrumentId.from_str("ETHUSDT-PERP.BINANCE")
    quote = m.QuoteTick(iid, m.Price("3400.10"), m.Price("3400.11"), m.Quantity("1.0"), m.Quantity("2.0"), 10, 11)
    lifecycle = m.NodeLifecycle(m.NodeState.INIT, m.NodeState.WIRED, m.LifecycleReason.CONFIGURED, 12)
    directory = tmp_path / "w"
    with log.EventLogWriter(str(directory)) as writer:
        writer.append(quote, seq=1, ts=11, source_id=2)
        writer.append(lifecycle, seq=2, ts=12)
    records = list(log.read(str(directory)))
    assert [(r.seq, r.ts, r.source_id, r.kind) for r in records] == [
        (1, 11, 2, "QuoteTick"),
        (2, 12, 0, "NodeLifecycle"),
    ]
    assert records[0].event == quote
    assert records[1].event == lifecycle
    assert log.event_text(quote).startswith("QuoteTick instrument_id=ETHUSDT-PERP.BINANCE")


def test_errors(tmp_path: Path) -> None:
    directory = tmp_path / "x"
    with log.EventLogWriter(str(directory)) as writer:
        with pytest.raises(TypeError):
            writer.append("not an event", seq=1, ts=1)
    with pytest.raises(ValueError, match="AlreadyExists"):
        log.EventLogWriter(str(directory))
    with pytest.raises(ValueError, match="NotFound"):
        log.EventLogReader(str(tmp_path))
    with pytest.raises(ValueError):
        log.fingerprint(str(directory), "everything")
