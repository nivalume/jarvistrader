//! Strategies by name: a `[[strategies]]` entry's `impl = "rust:<name>"` picks a registered
//! factory, which builds the strategy from the entry's parameters. `py:` strategies arrive with
//! the Python host (docs/rust-plan.md R4, second step); `cpp:` ones belong to the C++ tree.

use kernel_core::Status;
use strategy::Strategy;

use crate::config::StrategyConfig;
use crate::probe::Probe;
use crate::{NodeError, NodeResult};

/// Builds a strategy from its configuration entry.
pub type StrategyFactory = fn(&StrategyConfig) -> NodeResult<Box<dyn Strategy>>;

#[derive(Clone, Default)]
pub struct Registry {
    entries: Vec<(&'static str, StrategyFactory)>,
}

impl std::fmt::Debug for Registry {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_list().entries(self.entries.iter().map(|(n, _)| n)).finish()
    }
}

impl Registry {
    /// The strategies this tree ships: `rust:Probe`.
    #[must_use]
    pub fn with_builtins() -> Self {
        let mut r = Self::default();
        r.register("Probe", Probe::create);
        r
    }

    /// Adds (or replaces) `name`.
    pub fn register(&mut self, name: &'static str, factory: StrategyFactory) {
        self.entries.retain(|(n, _)| *n != name);
        self.entries.push((name, factory));
    }

    pub fn names(&self) -> impl Iterator<Item = &'static str> + '_ {
        self.entries.iter().map(|(n, _)| *n)
    }

    /// The strategy of one `[[strategies]]` entry.
    pub fn create(&self, entry: &StrategyConfig) -> NodeResult<Box<dyn Strategy>> {
        let (scheme, name) = entry.impl_.split_once(':').unwrap_or(("", entry.impl_.as_str()));
        match scheme {
            "rust" => {
                let factory =
                    self.entries.iter().find(|(n, _)| *n == name).map(|(_, f)| *f).ok_or_else(
                        || {
                            NodeError::new(
                                Status::NotFound,
                                format!(
                                    "strategies: no Rust strategy named \"{name}\" (known: {})",
                                    self.names().collect::<Vec<_>>().join(", ")
                                ),
                            )
                        },
                    )?;
                factory(entry)
            }
            "py" => Err(NodeError::new(
                Status::UnsupportedMessage,
                format!(
                    "strategies: \"{}\" needs the Python host, which arrives with R4's second step",
                    entry.impl_
                ),
            )),
            "cpp" => Err(NodeError::new(
                Status::UnsupportedMessage,
                format!("strategies: \"{}\" is a C++ strategy; this is the Rust tree", entry.impl_),
            )),
            _ => Err(NodeError::new(
                Status::InvalidArgument,
                format!("strategies: impl \"{}\" is not rust:, py: or cpp:", entry.impl_),
            )),
        }
    }

    /// One strategy per `[[strategies]]` entry, in order.
    pub fn create_all(&self, entries: &[StrategyConfig]) -> NodeResult<Vec<Box<dyn Strategy>>> {
        entries.iter().map(|e| self.create(e)).collect()
    }
}
