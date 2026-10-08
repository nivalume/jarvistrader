//! `jarvis-rs`: the Rust tree's command-line tool. It takes the C++ `jarvis` tool's subcommands as
//! their layers land (docs/rust-plan.md); the ones below are what exists today.
#![forbid(unsafe_code)]

use std::io::{self, Read};
use std::process::ExitCode;

use jarvis_core::sha256::{hex, Sha256};

fn usage() -> ExitCode {
    eprintln!(
        "usage: jarvis-rs <command>\n\
         \n\
         commands:\n\
         \x20 build-info          print the crate version and the kernel contract\n\
         \x20 sha256 [FILE]       SHA-256 of a file or standard input, as the fingerprint tool prints it\n\
         \x20 crc32c [FILE]       CRC-32C of a file or standard input (hex)\n\
         \n\
         pending (docs/rust-plan.md): corpus, fingerprint, dump, roundtrip, config, replay, report"
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

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("build-info") => {
            println!("jarvis-rs {}", env!("CARGO_PKG_VERSION"));
            println!("kernel: no_std, forbid(unsafe_code), no floating point, overflow-checks on");
            println!("profile: {}", if cfg!(debug_assertions) { "debug" } else { "release" });
            ExitCode::SUCCESS
        }
        Some("sha256") => match read_input(args.get(1).map(String::as_str)) {
            Ok(data) => {
                let digest = hex(&Sha256::digest(&data));
                println!("{}", core::str::from_utf8(&digest).unwrap_or("?"));
                ExitCode::SUCCESS
            }
            Err(e) => {
                eprintln!("jarvis-rs: {e}");
                ExitCode::FAILURE
            }
        },
        Some("crc32c") => match read_input(args.get(1).map(String::as_str)) {
            Ok(data) => {
                println!("{:08x}", jarvis_core::crc32c::crc32c(&data));
                ExitCode::SUCCESS
            }
            Err(e) => {
                eprintln!("jarvis-rs: {e}");
                ExitCode::FAILURE
            }
        },
        _ => usage(),
    }
}
