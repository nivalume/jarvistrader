//! The model's encodings, exercised over the deterministic corpus: the wire encoding is canonical
//! (decode after encode is the identity, encode after decode reproduces the bytes), the log framing
//! detects truncation and corruption, fingerprints depend on records alone, and the corpus itself
//! is a pure function of the seed.

use corpus::Corpus;
use kernel_core::{EventKey, Status};
use model::event::EventKind;
use model::log::{fingerprint, Fingerprinter, LogHeader, LogReader, LogWriter};
use model::{Event, Wire};

fn header() -> LogHeader {
    LogHeader::new([7u8; 32], 42, "mm01", "backtest").unwrap()
}

#[test]
fn corpus_covers_every_event_kind_and_order_variant() {
    let corpus = Corpus::new(7);
    let mut kinds = std::collections::BTreeSet::new();
    let mut order_tags = std::collections::BTreeSet::new();
    for record in corpus.records(2000) {
        let (_, event) = record.unwrap();
        kinds.insert(event.kind());
        if let Event::Order(o) = &event {
            order_tags.insert(o.tag());
        }
    }
    assert_eq!(kinds.len(), EventKind::ALL.len());
    assert_eq!(order_tags.len(), 17);
}

#[test]
fn corpus_is_a_pure_function_of_the_seed() {
    let a: Vec<_> = Corpus::new(42).records(300).map(Result::unwrap).collect();
    let b: Vec<_> = Corpus::new(42).records(300).map(Result::unwrap).collect();
    let c: Vec<_> = Corpus::new(43).records(300).map(Result::unwrap).collect();
    assert_eq!(a, b);
    assert_ne!(a, c);
    assert_eq!(
        Corpus::new(42).record(150).unwrap(),
        a[149],
        "a record can be recomputed on its own"
    );
    for w in a.windows(2) {
        assert!(w[0].0 < w[1].0, "keys strictly increase");
    }
}

#[test]
fn wire_encoding_is_canonical_over_the_corpus() {
    for (seed, count) in [(1u64, 400u64), (99, 400)] {
        for record in Corpus::new(seed).records(count) {
            let (_, event) = record.unwrap();
            let bytes = event.to_wire();
            let back = Event::from_wire(&bytes).unwrap_or_else(|e| panic!("{e}: {event:?}"));
            assert_eq!(back, event);
            assert_eq!(back.to_wire(), bytes, "re-encoding reproduces the bytes");
            // Any strict prefix is truncated, never a different value.
            for cut in [bytes.len() - 1, bytes.len() / 2, 2, 0] {
                assert!(
                    Event::from_wire(&bytes[..cut]).is_err(),
                    "prefix of {cut} decodes: {event:?}"
                );
            }
            let mut longer = bytes.clone();
            longer.push(0);
            assert_eq!(Event::from_wire(&longer).err(), Some(Status::InvalidArgument));
        }
    }
}

#[test]
fn log_round_trips_and_detects_truncation_and_corruption() {
    let records: Vec<_> = Corpus::new(3).records(50).map(Result::unwrap).collect();
    let mut writer = LogWriter::new(&header());
    for (key, event) in &records {
        writer.append(*key, event).unwrap();
    }
    assert_eq!(writer.records(), 50);
    let bytes = writer.into_bytes();

    let (h, reader) = LogReader::open(&bytes).unwrap();
    assert_eq!(h, header());
    let read: Vec<_> =
        reader.map(|r| r.unwrap()).map(|r| (r.key, r.input().unwrap().clone())).collect();
    assert_eq!(read, records);

    // Truncated anywhere in the last record: the first 49 read, then Truncated.
    let last_start = {
        let (_, mut r) = LogReader::open(&bytes).unwrap();
        for _ in 0..49 {
            r.next_record().unwrap();
        }
        r.position()
    };
    for cut in [last_start + 1, last_start + 4, last_start + 10, bytes.len() - 1] {
        let (_, mut r) = LogReader::open(&bytes[..cut]).unwrap();
        let mut n = 0;
        let err = loop {
            match r.next_record() {
                Ok(Some(_)) => n += 1,
                Ok(None) => break None,
                Err(e) => break Some(e),
            }
        };
        assert_eq!((n, err), (49, Some(Status::Truncated)), "cut at {cut}");
    }
    // A flipped byte in a body: ChecksumMismatch for that record.
    let mut corrupted = bytes.clone();
    let idx = last_start + 30;
    corrupted[idx] ^= 0x01;
    let (_, mut r) = LogReader::open(&corrupted).unwrap();
    for _ in 0..49 {
        r.next_record().unwrap();
    }
    assert_eq!(r.next_record().err(), Some(Status::ChecksumMismatch));
    // A damaged header.
    let mut bad_header = bytes.clone();
    bad_header[12] ^= 0x01;
    assert_eq!(LogReader::open(&bad_header).err(), Some(Status::ChecksumMismatch));
    assert_eq!(LogReader::open(b"not a log").err(), Some(Status::ParseError));
}

#[test]
fn fingerprint_ignores_header_text_and_segmenting_but_not_records() {
    let records: Vec<_> = Corpus::new(5).records(60).map(Result::unwrap).collect();
    let write = |h: &LogHeader, items: &[(EventKey, Event)]| {
        let mut w = LogWriter::new(h);
        for (k, e) in items {
            w.append(*k, e).unwrap();
        }
        w.into_bytes()
    };
    let a = fingerprint(&write(&header(), &records)).unwrap();
    let mut other = header();
    other.build = kernel_core::FixedString::from_text("another compiler").unwrap();
    let b = fingerprint(&write(&other, &records)).unwrap();
    assert_eq!(a, b);
    assert_eq!(a.records, 60);
    // The same records through the incremental fingerprinter, in two segments.
    let mut fp = Fingerprinter::new();
    for (k, e) in &records {
        fp.add(*k, e);
    }
    assert_eq!(fp.finish(), a);
    // One record changed: a different digest.
    let mut changed = records.clone();
    changed[10].0.seq += 1;
    assert_ne!(fingerprint(&write(&header(), &changed)).unwrap(), a);
    assert_eq!(a.to_string().len(), "60 ".len() + 64);
}

#[test]
fn seed7_corpus_fingerprint_is_golden() {
    // The Rust tree's own golden (rust/tests/golden/corpus_seed7.fingerprint). It changes only
    // with a deliberate change to the corpus generator or an encoding, which bumps SCHEMA_VERSION.
    let expected = include_str!("../../../../tests/golden/corpus_seed7.fingerprint").trim();
    let mut fp = Fingerprinter::new();
    for record in Corpus::new(7).records(2000) {
        let (k, e) = record.unwrap();
        fp.add(k, &e);
    }
    assert_eq!(fp.finish().to_string(), expected);
}
