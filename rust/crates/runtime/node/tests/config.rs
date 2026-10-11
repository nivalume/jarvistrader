//! The canonical configuration text and hash equal the C++ node's goldens byte for byte.

use node::{config_report, format_errors, parse_config, ConfigOverrides};

const NODE_TOML: &str = include_str!("../../../../../examples/config/node.toml");
const CANONICAL: &str =
    include_str!("../../../../../tests/golden/node_config/expected.canonical.txt");
const OVERRIDDEN: &str =
    include_str!("../../../../../tests/golden/node_config/expected.overridden.txt");

fn parse(overrides: &ConfigOverrides) -> node::NodeConfig {
    match parse_config(NODE_TOML, overrides) {
        Ok(c) => c,
        Err(errors) => panic!("{}", format_errors("node.toml", &errors)),
    }
}

#[test]
fn canonical_text_matches_the_golden() {
    let config = parse(&ConfigOverrides::default());
    assert_eq!(config_report(&config), CANONICAL);
}

#[test]
fn overrides_match_the_golden() {
    let overrides = ConfigOverrides {
        env: Some("sandbox".to_string()),
        sets: vec!["venues.BINANCE_USDM.endpoint=testnet".to_string(), "node.seed=7".to_string()],
    };
    let config = parse(&overrides);
    assert_eq!(config_report(&config), OVERRIDDEN);
}
