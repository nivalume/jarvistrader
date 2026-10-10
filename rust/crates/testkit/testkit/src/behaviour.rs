//! Reader for the behaviour files `tools/tla/behaviours.py` writes (docs/architecture.md 18.2),
//! and the forward trace driver that replays them through an implementation:
//!
//! ```text
//! spec <Spec> num <N> depth <D> seed <S>
//! const <Name> <value>
//! behaviour <k>
//! step <Action> <args...> | <var>=<value> ...
//! end
//! ```
//!
//! A [`Replayer`] maps each action to the implementation's inputs and projects the
//! implementation's state on the spec's variables; [`replay`] compares after every step and
//! fails on the first difference, naming the behaviour, the step and the action, and when an
//! action of the spec never appeared in the file.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::Path;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Step {
    pub action: String,
    pub args: Vec<String>,
    pub vars: BTreeMap<String, String>,
    pub line: usize,
}

impl Step {
    /// The rendered value of a variable; panics when the file does not keep it.
    #[must_use]
    pub fn var(&self, name: &str) -> &str {
        self.vars.get(name).unwrap_or_else(|| panic!("line {}: no variable {name}", self.line))
    }
    #[must_use]
    pub fn integer(&self, name: &str) -> i64 {
        to_integer(self.var(name))
            .unwrap_or_else(|| panic!("line {}: {name} is not an integer", self.line))
    }
    #[must_use]
    pub fn boolean(&self, name: &str) -> bool {
        match self.var(name) {
            "TRUE" => true,
            "FALSE" => false,
            other => panic!("line {}: {name}={other} is not a boolean", self.line),
        }
    }
    #[must_use]
    pub fn set(&self, name: &str) -> BTreeSet<String> {
        to_set(self.var(name))
    }
    #[must_use]
    pub fn int_map(&self, name: &str) -> BTreeMap<i64, i64> {
        to_int_map(self.var(name))
    }
    /// Argument `i` as an integer.
    #[must_use]
    pub fn arg(&self, i: usize) -> i64 {
        let text =
            self.args.get(i).unwrap_or_else(|| panic!("line {}: no argument {i}", self.line));
        to_integer(text)
            .unwrap_or_else(|| panic!("line {}: argument {i} is not an integer", self.line))
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Behaviour {
    pub number: usize,
    pub steps: Vec<Step>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BehaviourFile {
    pub spec: String,
    pub constants: BTreeMap<String, String>,
    pub behaviours: Vec<Behaviour>,
}

impl BehaviourFile {
    #[must_use]
    pub fn constant(&self, name: &str) -> i64 {
        to_integer(self.constants.get(name).unwrap_or_else(|| panic!("no constant {name}")))
            .unwrap_or_else(|| panic!("constant {name} is not an integer"))
    }

    /// Parses the file at `path`; panics with the line on malformed input.
    #[must_use]
    pub fn read(path: &Path) -> Self {
        let text =
            std::fs::read_to_string(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        Self::parse(&text)
    }

    #[must_use]
    pub fn parse(text: &str) -> Self {
        let mut file = Self::default();
        let mut current: Option<Behaviour> = None;
        for (n, raw) in text.lines().enumerate() {
            let line = n + 1;
            let l = raw.trim();
            if l.is_empty() || l.starts_with('#') {
                continue;
            }
            let (head, rest) = l.split_once(' ').unwrap_or((l, ""));
            match head {
                "spec" => file.spec = rest.split(' ').next().unwrap_or("").to_string(),
                "const" => {
                    let (name, value) =
                        rest.split_once(' ').unwrap_or_else(|| panic!("line {line}: const"));
                    file.constants.insert(name.to_string(), value.trim().to_string());
                }
                "behaviour" => {
                    assert!(current.is_none(), "line {line}: behaviour inside a behaviour");
                    current = Some(Behaviour {
                        number: rest
                            .trim()
                            .parse()
                            .unwrap_or_else(|_| panic!("line {line}: number")),
                        steps: Vec::new(),
                    });
                }
                "step" => {
                    let b = current
                        .as_mut()
                        .unwrap_or_else(|| panic!("line {line}: step outside a behaviour"));
                    let (action_part, vars_part) = rest.split_once('|').unwrap_or((rest, ""));
                    let mut words = action_part.split_whitespace();
                    let action = words
                        .next()
                        .unwrap_or_else(|| panic!("line {line}: no action"))
                        .to_string();
                    let args = words.map(str::to_string).collect();
                    let mut vars = BTreeMap::new();
                    for kv in vars_part.split_whitespace() {
                        let (k, v) =
                            kv.split_once('=').unwrap_or_else(|| panic!("line {line}: {kv}"));
                        vars.insert(k.to_string(), v.to_string());
                    }
                    b.steps.push(Step { action, args, vars, line });
                }
                "end" => {
                    let b = current
                        .take()
                        .unwrap_or_else(|| panic!("line {line}: end outside a behaviour"));
                    file.behaviours.push(b);
                }
                other => panic!("line {line}: unknown line kind {other}"),
            }
        }
        assert!(current.is_none(), "unterminated behaviour");
        file
    }
}

#[must_use]
pub fn to_integer(text: &str) -> Option<i64> {
    text.trim().parse().ok()
}

/// `{a,b,c}` as a set of its rendered elements; `{}` is empty.
#[must_use]
pub fn to_set(text: &str) -> BTreeSet<String> {
    let inner = text.trim().strip_prefix('{').and_then(|t| t.strip_suffix('}')).unwrap_or(text);
    inner.split(',').map(str::trim).filter(|s| !s.is_empty()).map(str::to_string).collect()
}

/// An integer function as rendered: `<a,b>` (a sequence, keys from 1) or `{k:v,...}`; `{}` is
/// empty. Keys that are not integers (`t1`) are numbered by their trailing digits.
#[must_use]
pub fn to_int_map(text: &str) -> BTreeMap<i64, i64> {
    let t = text.trim();
    let mut out = BTreeMap::new();
    if let Some(inner) = t.strip_prefix('<').and_then(|s| s.strip_suffix('>')) {
        for (i, v) in inner.split(',').filter(|s| !s.is_empty()).enumerate() {
            out.insert(i as i64 + 1, to_integer(v).expect("sequence value"));
        }
        return out;
    }
    let inner = t.strip_prefix('{').and_then(|s| s.strip_suffix('}')).unwrap_or(t);
    for kv in inner.split(',').filter(|s| !s.is_empty()) {
        let (k, v) = kv.split_once(':').expect("k:v");
        let key = to_integer(k).unwrap_or_else(|| {
            let digits: String = k.chars().filter(char::is_ascii_digit).collect();
            digits.parse().expect("key")
        });
        out.insert(key, to_integer(v).expect("map value"));
    }
    out
}

/// Collects the first differences between the spec's state and the implementation's.
#[derive(Clone, Debug, Default)]
pub struct Diff {
    text: String,
}

impl Diff {
    #[allow(clippy::needless_pass_by_value)] // values are small and read once
    pub fn expect<A: PartialEq<B> + std::fmt::Debug, B: std::fmt::Debug>(
        &mut self,
        var: &str,
        spec: A,
        implementation: B,
    ) {
        if spec != implementation {
            let _ =
                writeln!(self.text, "  {var}: spec {spec:?}, implementation {implementation:?}");
        }
    }
    pub fn note(&mut self, line: &str) {
        let _ = writeln!(self.text, "  {line}");
    }
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.text.is_empty()
    }
    #[must_use]
    pub fn text(&self) -> &str {
        &self.text
    }
}

/// An implementation under forward trace validation.
pub trait Replayer {
    /// The spec's actions; every one must appear in the file.
    fn actions(&self) -> &'static [&'static str];
    /// Builds the initial state from the `Init` step; returns the differences.
    fn start(&mut self, init: &Step) -> Diff;
    /// Applies one step; returns the differences after it.
    fn step(&mut self, step: &Step) -> Diff;
}

/// Replays every behaviour of `file`, building a fresh replayer per behaviour; panics on the
/// first difference or when an action never appeared.
pub fn replay<R: Replayer>(file: &BehaviourFile, mut make: impl FnMut(&BehaviourFile) -> R) {
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut actions: &[&str] = &[];
    for b in &file.behaviours {
        let mut r = make(file);
        actions = r.actions();
        let (init, rest) =
            b.steps.split_first().unwrap_or_else(|| panic!("behaviour {} is empty", b.number));
        assert_eq!(init.action, "Init", "behaviour {}: first step is not Init", b.number);
        let diff = r.start(init);
        assert!(
            diff.is_empty(),
            "{} behaviour {} step 1 (Init, line {}):\n{}",
            file.spec,
            b.number,
            init.line,
            diff.text()
        );
        for (i, s) in rest.iter().enumerate() {
            seen.insert(s.action.clone());
            let diff = r.step(s);
            assert!(
                diff.is_empty(),
                "{} behaviour {} step {} ({} {}, line {}):\n{}",
                file.spec,
                b.number,
                i + 2,
                s.action,
                s.args.join(" "),
                s.line,
                diff.text()
            );
        }
    }
    for a in actions {
        assert!(seen.contains(*a), "{}: action {a} never appears in the file", file.spec);
    }
}

/// `tests/trace/behaviours/<spec>.txt` of the repository, from a crate's manifest directory.
#[must_use]
pub fn behaviours_path(manifest_dir: &str, spec: &str) -> std::path::PathBuf {
    Path::new(manifest_dir).join("../../../../tests/trace/behaviours").join(format!("{spec}.txt"))
}

/// `specs/tla/<spec>.tla` of the repository.
#[must_use]
pub fn spec_path(manifest_dir: &str, spec: &str) -> std::path::PathBuf {
    Path::new(manifest_dir).join("../../../../specs/tla").join(format!("{spec}.tla"))
}

/// The tuples between `\* BEGIN <name>` and `\* END <name>` of a TLA+ module, each `<<a, b, c>>`
/// as its string elements with quotes stripped (`TRUE` and `FALSE` stay as text).
#[must_use]
pub fn spec_tuples(text: &str, name: &str) -> Vec<Vec<String>> {
    let begin = format!("\\* BEGIN {name}");
    let end = format!("\\* END {name}");
    let mut inside = false;
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with(&begin) {
            inside = true;
            continue;
        }
        if line.starts_with(&end) {
            break;
        }
        if !inside {
            continue;
        }
        let mut rest = line;
        while let Some(start) = rest.find("<<") {
            let after = &rest[start + 2..];
            let Some(stop) = after.find(">>") else { break };
            let tuple =
                after[..stop].split(',').map(|s| s.trim().trim_matches('"').to_string()).collect();
            out.push(tuple);
            rest = &after[stop + 2..];
        }
    }
    out
}
