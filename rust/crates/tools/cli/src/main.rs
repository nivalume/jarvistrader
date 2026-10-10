//! `jarvis-rs`: the Rust tree's command-line tool. Subcommands arrive with their layers
//! (docs/rust-plan.md); the ones below are what exists today.
#![forbid(unsafe_code)]

use std::io::{self, BufRead, Read, Write};
use std::process::ExitCode;

use corpus::Corpus;
use kernel_core::sha256::{hex, Sha256};
use model::bar::BarType;
use model::log::{fingerprint, LogHeader, LogReader, LogWriter, RecordBody};
use model::{InstrumentId, Money, Price, Quantity};

fn usage() -> ExitCode {
    eprintln!(
        "usage: jarvis-rs <command> [options]\n\
         \n\
         commands:\n\
         \x20 build-info                               the crate version and the kernel contract\n\
         \x20 corpus --seed N --events M --out FILE    write the deterministic corpus as an event log\n\
         \x20 fingerprint FILE                         '<records> <sha256>' of an event log's records\n\
         \x20 dump FILE [--limit N]                    print records as text\n\
         \x20 roundtrip                                parse each stdin line as a model value and print its text form\n\
         \x20 sha256 [FILE] | crc32c [FILE]            digests of a file or standard input\n\
         \n\
         pending (docs/rust-plan.md): config, replay, report"
    );
    ExitCode::from(2)
}

fn read_input(path: Option<&str>) -> io::Result<Vec<u8>> {
    if let Some(p) = path {
        return std::fs::read(p);
    }
    let mut buf = Vec::new();
    io::stdin().read_to_end(&mut buf)?;
    Ok(buf)
}

fn option<'a>(args: &'a [String], name: &str) -> Option<&'a str> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).map(String::as_str)
}

fn fail(message: &str) -> ExitCode {
    eprintln!("jarvis-rs: {message}");
    ExitCode::FAILURE
}

fn corpus(args: &[String]) -> ExitCode {
    let Some(seed) = option(args, "--seed").and_then(|s| s.parse::<u64>().ok()) else {
        return fail("corpus needs --seed N");
    };
    let Some(events) = option(args, "--events").and_then(|s| s.parse::<u64>().ok()) else {
        return fail("corpus needs --events M");
    };
    let Some(out) = option(args, "--out") else { return fail("corpus needs --out FILE") };
    let header = match LogHeader::new(Sha256::digest(b"corpus"), seed, "corpus", "corpus") {
        Ok(h) => h,
        Err(e) => return fail(&e.to_string()),
    };
    let mut writer = LogWriter::new(&header);
    for record in Corpus::new(seed).records(events) {
        match record.and_then(|(key, event)| writer.append(key, &event)) {
            Ok(()) => {}
            Err(e) => return fail(&format!("generating the corpus: {e}")),
        }
    }
    match std::fs::write(out, writer.into_bytes()) {
        Ok(()) => {
            println!("{events} records to {out}");
            ExitCode::SUCCESS
        }
        Err(e) => fail(&e.to_string()),
    }
}

fn fingerprint_cmd(args: &[String]) -> ExitCode {
    let Some(path) = args.first() else { return fail("fingerprint needs FILE") };
    match std::fs::read(path)
        .map_err(|e| e.to_string())
        .and_then(|b| fingerprint(&b).map_err(|e| e.to_string()))
    {
        Ok(fp) => {
            println!("{fp}");
            ExitCode::SUCCESS
        }
        Err(e) => fail(&e),
    }
}

fn dump(args: &[String]) -> ExitCode {
    let Some(path) = args.first() else { return fail("dump needs FILE") };
    let limit = option(args, "--limit").and_then(|s| s.parse::<usize>().ok()).unwrap_or(usize::MAX);
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => return fail(&e.to_string()),
    };
    let (header, reader) = match LogReader::open(&bytes) {
        Ok(v) => v,
        Err(e) => return fail(&format!("reading the header: {e}")),
    };
    let stdout = io::stdout();
    let mut out = stdout.lock();
    let _ = writeln!(
        out,
        "# format {} schema {} seed {} node {} env {}",
        header.format_version, header.schema_version, header.seed, header.node_tag, header.env
    );
    for (n, record) in reader.enumerate() {
        if n >= limit {
            break;
        }
        match record {
            Ok(r) => {
                let _ = match &r.body {
                    RecordBody::Input(e) => writeln!(
                        out,
                        "{} {} {} {} {:?}",
                        r.key.seq,
                        r.key.ts.value(),
                        r.key.source_id,
                        e.kind().name(),
                        e
                    ),
                    RecordBody::Output(o) => writeln!(
                        out,
                        "{} {} {} out {} {:?}",
                        r.key.seq,
                        r.key.ts.value(),
                        r.key.source_id,
                        o.name(),
                        o
                    ),
                };
            }
            Err(e) => return fail(&format!("record {}: {e}", n + 1)),
        }
    }
    ExitCode::SUCCESS
}

/// Each line is `Type value`; the output is `Type text` with the value's canonical text form, or
/// `Type ERROR status`. Types: `Price`, `Quantity`, `Money`, `InstrumentId`, `BarType`.
fn roundtrip() -> ExitCode {
    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(e) => return fail(&e.to_string()),
        };
        let Some((kind, text)) = line.split_once(' ') else {
            println!("{line} ERROR ParseError");
            continue;
        };
        let shown: Result<String, kernel_core::Status> = match kind {
            "Price" => Price::parse(text).map(|v| v.to_string()),
            "Quantity" => Quantity::parse(text).map(|v| v.to_string()),
            "Money" => Money::parse(text).map(|v| v.to_string()),
            "InstrumentId" => InstrumentId::parse(text).map(|v| v.to_string()),
            "BarType" => BarType::parse(text).map(|v| v.to_string()),
            _ => {
                println!("{kind} ERROR UnsupportedMessage");
                continue;
            }
        };
        match shown {
            Ok(s) => println!("{kind} {s}"),
            Err(e) => println!("{kind} ERROR {e}"),
        }
    }
    ExitCode::SUCCESS
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let rest = args.get(1..).unwrap_or(&[]);
    match args.first().map(String::as_str) {
        Some("build-info") => {
            println!("jarvis-rs {}", env!("CARGO_PKG_VERSION"));
            println!("schema {}", model::event::SCHEMA_VERSION);
            println!("kernel: no_std, forbid(unsafe_code), no floating point, overflow-checks on");
            println!("profile: {}", if cfg!(debug_assertions) { "debug" } else { "release" });
            ExitCode::SUCCESS
        }
        Some("corpus") => corpus(rest),
        Some("fingerprint") => fingerprint_cmd(rest),
        Some("dump") => dump(rest),
        Some("roundtrip") => roundtrip(),
        Some("sha256") => match read_input(rest.first().map(String::as_str)) {
            Ok(data) => {
                let digest = hex(&Sha256::digest(&data));
                println!("{}", core::str::from_utf8(&digest).unwrap_or("?"));
                ExitCode::SUCCESS
            }
            Err(e) => fail(&e.to_string()),
        },
        Some("crc32c") => match read_input(rest.first().map(String::as_str)) {
            Ok(data) => {
                println!("{:08x}", kernel_core::crc32c::crc32c(&data));
                ExitCode::SUCCESS
            }
            Err(e) => fail(&e.to_string()),
        },
        _ => usage(),
    }
}
