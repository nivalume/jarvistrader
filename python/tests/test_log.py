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
    assert len({r.kind for r in records}) == 39  # every record kind

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


def test_output_records_round_trip(tmp_path: Path) -> None:
    from decimal import Decimal

    directory = tmp_path / "out"
    trade = m.TradeTick(
        instrument_id=m.InstrumentId.from_str("BTCUSDT-PERP.BINANCE"),
        price=m.Price("65000.1"),
        size=m.Quantity("0.010"),
        aggressor_side=m.AggressorSide.BUY,
        trade_id=m.TradeId("1"),
        ts_event=100,
        ts_init=100,
    )
    feature = m.FeatureUpdate(feature_id=3, value=Decimal("1.25"), ts_event=100, ts_init=100)
    record = m.StrategyRecord(strategy_index=1, tag="mid", value=Decimal("-0.5"), ts_init=100)
    with log.EventLogWriter(str(directory)) as writer:
        writer.append(trade, seq=1, ts=100)
        writer.append(feature, seq=1, ts=100, source_id=0)
        writer.append(record, seq=1, ts=100, source_id=1)
    records = list(log.read(str(directory)))
    assert [r.kind for r in records] == ["TradeTick", "FeatureUpdate", "StrategyRecord"]
    assert [r.is_output for r in records] == [False, True, True]
    assert records[1].event == feature
    assert records[2].event == record
    assert records[2].event.tag == "mid"
    assert log.fingerprint(str(directory), "inputs")[1] == 1
    assert log.fingerprint(str(directory), "outputs")[1] == 2
