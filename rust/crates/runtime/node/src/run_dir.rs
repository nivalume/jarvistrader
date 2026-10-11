//! A run directory (docs/architecture.md section 16.1) holds everything needed to replay a run:
//!
//! - `config.toml`: the configuration text exactly as the node loaded it;
//! - `run.toml`: the `--env` and `--set` overrides applied on top, the resulting config hash,
//!   and for a resumed run the run it continues;
//! - `events-*.jlog`: the run log, every input the node stepped, each followed by its outputs;
//! - `snapshot-*.jsnap`: `EngineState` snapshots.
//!
//! Replay rebuilds the configuration from the first two files, checks its hash against the log
//! header, and recomputes the outputs from the inputs.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use kernel_core::{FixedString, Status};
use model::log::LogHeader;
use toml_edit::{DocumentMut, Item};

use crate::build_info;
use crate::config::{
    config_hash, config_hash_hex, env_name, format_errors, parse_config, ConfigOverrides,
    NodeConfig,
};
use crate::event_log::{has_log, EventLogReadOptions, EventLogReader};
use crate::{NodeError, NodeResult};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RunManifest {
    /// Saved as `config.toml`.
    pub config_text: String,
    /// Where it was loaded from (informational).
    pub source: String,
    /// Saved in `run.toml`.
    pub overrides: ConfigOverrides,
    /// The earlier run a resumed run continues (informational).
    pub resumed_from: String,
}

fn set_text<const N: usize>(text: &str) -> FixedString<N> {
    let cut = text.char_indices().map(|(i, _)| i).find(|&i| i > N).unwrap_or(text.len());
    let mut end = cut.min(text.len());
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    FixedString::from_text(&text[..end]).unwrap_or_default()
}

/// What a Python host adds to the log header (docs/architecture.md section 16.1); empty for a
/// native node.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HeaderExtras {
    pub python_version: String,
    pub numpy_version: String,
    pub python_hash_seed: Option<u32>,
}

/// The log header of a run under `config`: seed, config hash and the build identity.
#[must_use]
pub fn run_header(config: &NodeConfig) -> LogHeader {
    run_header_with(config, &HeaderExtras::default())
}

/// `run_header` with a host's extras.
#[must_use]
pub fn run_header_with(config: &NodeConfig, extras: &HeaderExtras) -> LogHeader {
    let mut header = LogHeader::new(config_hash(config), config.node.seed, "", "")
        .unwrap_or_else(|_| unreachable!("empty tags fit"));
    header.node_tag = set_text(&config.node.id);
    header.env = set_text(env_name(config.node.env));
    header.build = set_text(&build_info::build_text());
    header.platform = set_text(&build_info::platform_text());
    header.python_version = set_text(&extras.python_version);
    header.numpy_version = set_text(&extras.numpy_version);
    header.python_hash_seed = extras.python_hash_seed;
    header
}

/// A TOML basic string.
fn toml_string(text: &str) -> String {
    let mut out = String::from("\"");
    for c in text.chars() {
        if c == '"' || c == '\\' {
            out.push('\\');
        }
        out.push(c);
    }
    out.push('"');
    out
}

fn manifest_text(config: &NodeConfig, manifest: &RunManifest) -> String {
    let mut out = String::from(
        "# Written by jarvis when the run started; replay rebuilds the configuration from\n\
         # config.toml with these overrides and checks the hash against the log header.\n",
    );
    let _ = writeln!(out, "source = {}", toml_string(&manifest.source));
    if let Some(env) = &manifest.overrides.env {
        let _ = writeln!(out, "env = {}", toml_string(env));
    }
    out.push_str("sets = [");
    for (i, set) in manifest.overrides.sets.iter().enumerate() {
        if i > 0 {
            out.push_str(", ");
        }
        out.push_str(&toml_string(set));
    }
    out.push_str("]\n");
    let _ = writeln!(out, "config_hash = {}", toml_string(&config_hash_hex(config)));
    if !manifest.resumed_from.is_empty() {
        let _ = writeln!(out, "resumed_from = {}", toml_string(&manifest.resumed_from));
    }
    out
}

/// The run directories' names order by their UTC stamp, then by the `-2`, `-3`, ... a taken
/// name got.
fn run_id_key(id: &str) -> (String, u64) {
    match id.split_once('-') {
        Some((stamp, n)) => (stamp.to_string(), n.parse().unwrap_or(1)),
        None => (id.to_string(), 1),
    }
}

/// Creates the run directory and writes `config.toml` and `run.toml`. The directory is `out`
/// when given (it must not contain a log yet), else `persistence.dir` with `{node_id}` replaced
/// by `node.id` and `{run_id}` by the UTC start time (`YYYYmmddTHHMMSSZ`, with `-2`, `-3`, ...
/// when taken). The name is the only thing wall-clock time affects.
pub fn create_run_directory(
    config: &NodeConfig,
    manifest: &RunManifest,
    out: Option<&Path>,
) -> NodeResult<PathBuf> {
    let dir = if let Some(out) = out {
        if has_log(out) {
            return Err(NodeError::new(
                Status::AlreadyExists,
                format!("{} already holds a run log", out.display()),
            ));
        }
        out.to_path_buf()
    } else {
        {
            let base = config.persistence.dir.replace("{node_id}", &config.node.id);
            let stamp = sys::run_id(sys::wall_now());
            let mut attempt = 1u32;
            loop {
                let run_id =
                    if attempt == 1 { stamp.clone() } else { format!("{stamp}-{attempt}") };
                let dir = PathBuf::from(base.replace("{run_id}", &run_id));
                if !dir.exists() {
                    break dir;
                }
                if !base.contains("{run_id}") {
                    if has_log(&dir) {
                        return Err(NodeError::new(
                            Status::AlreadyExists,
                            format!(
                                "{} already holds a run log (persistence.dir has no {{run_id}})",
                                dir.display()
                            ),
                        ));
                    }
                    break dir;
                }
                attempt += 1;
            }
        }
    };
    std::fs::create_dir_all(&dir).map_err(|e| {
        NodeError::new(Status::IoError, format!("cannot create {}: {e}", dir.display()))
    })?;
    let write = |name: &str, text: &str| {
        std::fs::write(dir.join(name), text).map_err(|_| {
            NodeError::new(
                Status::IoError,
                format!("cannot write the run manifest in {}", dir.display()),
            )
        })
    };
    write("config.toml", &manifest.config_text)?;
    write("run.toml", &manifest_text(config, manifest))?;
    Ok(dir)
}

/// The run directories of this node where `persistence.dir` puts them (`{node_id}` replaced,
/// any `{run_id}`), oldest first. Only directories holding a run log count.
#[must_use]
pub fn node_runs(config: &NodeConfig) -> Vec<PathBuf> {
    const RUN_ID: &str = "{run_id}";
    let base = config.persistence.dir.replace("{node_id}", &config.node.id);
    let Some(at) = base.find(RUN_ID) else {
        let dir = PathBuf::from(&base);
        return if has_log(&dir) { vec![dir] } else { Vec::new() };
    };
    // The path component holding {run_id}: its parent is listed, and the entries matching the
    // component's text around {run_id} are the runs.
    let slash = base[..at].rfind('/');
    let begin = slash.map_or(0, |s| s + 1);
    let end = base[at..].find('/').map_or(base.len(), |e| at + e);
    let parent = slash.map_or_else(|| ".".to_string(), |s| base[..=s].to_string());
    let prefix = &base[begin..at];
    let suffix = &base[at + RUN_ID.len()..end];
    let rest = &base[end..];
    let mut runs: Vec<(String, PathBuf)> = Vec::new();
    if let Ok(entries) = std::fs::read_dir(&parent) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.len() <= prefix.len() + suffix.len()
                || !name.starts_with(prefix)
                || !name.ends_with(suffix)
            {
                continue;
            }
            let id = name[prefix.len()..name.len() - suffix.len()].to_string();
            let dir =
                PathBuf::from(format!("{}{}{}", &base[..begin], name, rest.replace(RUN_ID, &id)));
            if has_log(&dir) {
                runs.push((id, dir));
            }
        }
    }
    runs.sort_by_key(|(id, _)| run_id_key(id));
    runs.into_iter().map(|(_, dir)| dir).collect()
}

/// The configuration a run was made with, checked against the log header's config hash.
pub fn load_run_config(directory: &Path) -> NodeResult<NodeConfig> {
    let shown = directory.display();
    let (Ok(manifest), Ok(text)) = (
        sys::read_text(&directory.join("run.toml")),
        sys::read_text(&directory.join("config.toml")),
    ) else {
        return Err(NodeError::new(
            Status::NotFound,
            format!("{shown} is not a run directory (config.toml or run.toml is missing)"),
        ));
    };
    let parsed: DocumentMut = manifest.parse().map_err(|e: toml_edit::TomlError| {
        NodeError::new(Status::ParseError, format!("{shown}/run.toml: {}", e.message()))
    })?;
    let mut overrides = ConfigOverrides::default();
    if let Some(env) = parsed.get("env").and_then(Item::as_str) {
        overrides.env = Some(env.to_string());
    }
    if let Some(sets) = parsed.get("sets").and_then(Item::as_array) {
        for v in sets {
            if let Some(s) = v.as_str() {
                overrides.sets.push(s.to_string());
            }
        }
    }
    let source = format!("{shown}/config.toml");
    let config = parse_config(&text, &overrides).map_err(|errors| {
        NodeError::new(Status::InvalidArgument, format_errors(&source, &errors))
    })?;
    let reader = EventLogReader::open(directory, EventLogReadOptions::default())
        .map_err(|s| NodeError::new(s, format!("{shown}: cannot open the run log: {s}")))?;
    if reader.header().config_hash != config_hash(&config) {
        return Err(NodeError::new(
            Status::InvalidState,
            format!("{shown}: the configuration does not match the log header's config hash"),
        ));
    }
    Ok(config)
}
