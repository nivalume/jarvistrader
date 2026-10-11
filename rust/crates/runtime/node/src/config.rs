//! Typed node configuration (docs/architecture.md section 4.2). The TOML text is parsed here, in
//! the shell; unknown keys, wrong types and invalid values are errors, every one of them reported
//! with its path and line. Floating-point values are rejected everywhere: decimals are written as
//! strings so they parse exactly into fixed point.
//!
//! The canonical form is one `path = value` line per field, defaults included, in a fixed order,
//! strategy parameters sorted by key. The hashed part covers everything that changes what the
//! kernel computes; the operational part (`node.env`, `[data]`, venue endpoints and credential
//! references, `[persistence]`, `[telemetry]`, `[admin]`, `[threads]`, `[network]`) says where
//! inputs come from and where outputs go. The text and the hash are the C++ node's byte for byte
//! (`tests/golden/node_config`), so a log either tree wrote replays under the other.

use std::collections::BTreeSet;
use std::fmt::Write as _;

use kernel_core::sha256::{hex, Digest, Sha256};
use kernel_core::time::{format_rfc3339, parse_rfc3339, RFC3339_MAX_LENGTH};
use kernel_core::{Result, Status, UnixNanos};
use model::enums::{StopMode, TradingState};
use model::{InstrumentId, Money, StrategyId};
use toml_edit::{DocumentMut, Item, TableLike, Value};

// ---- the configuration ------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Env {
    Backtest,
    Sandbox,
    Live,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AccountMode {
    OneWay,
    Hedge,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OmsKind {
    Netting,
    Hedging,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Endpoint {
    Prod,
    Testnet,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FillModel {
    TopOfBook,
    QueuePosition,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelfTradePrevention {
    None,
    ExpireTaker,
    ExpireMaker,
    ExpireBoth,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Codec {
    Json,
    Sbe,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OnStrategyError {
    HaltStrategy,
    HaltNode,
    Ignore,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PersistenceMode {
    None,
    Async,
    Barrier,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RawFrames {
    Off,
    On,
    Sampled,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Capacity {
    pub orders: u32,
    pub instruments: u32,
    pub batch: u32,
    pub timers: u32,
    pub strategies: u32,
}

impl Default for Capacity {
    fn default() -> Self {
        Self { orders: 4096, instruments: 64, batch: 1024, timers: 256, strategies: 8 }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NodeSection {
    /// Also the `ClientOrderId` node tag: 1-8 ASCII letters or digits.
    pub id: String,
    pub env: Env,
    pub seed: u64,
    pub strict_determinism: bool,
    pub capacity: Capacity,
    pub shutdown: StopMode,
    pub shutdown_timeout_ms: u32,
    /// Market data that is up but silent this long counts as stale; 0: no check.
    pub market_data_stale_ms: u32,
}

impl Default for NodeSection {
    fn default() -> Self {
        Self {
            id: String::new(),
            env: Env::Backtest,
            seed: 0,
            strict_determinism: true,
            capacity: Capacity::default(),
            shutdown: StopMode::CancelAllThenExit,
            shutdown_timeout_ms: 10_000,
            market_data_stale_ms: 10_000,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TimeRange {
    pub start: UnixNanos,
    pub end: UnixNanos,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataStream {
    pub venue: String,
    pub instruments: Vec<InstrumentId>,
    pub streams: Vec<String>,
    pub codec: Codec,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DataSection {
    pub catalog: String,
    pub range: Option<TimeRange>,
    pub streams: Vec<DataStream>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Latency {
    pub feed_ns: u64,
    pub out_ns: u64,
    pub in_ns: u64,
    pub jitter_ns: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SimSection {
    pub fill_model: FillModel,
    pub latency: Latency,
    /// Empty: `binance_usdm_vip0`.
    pub fee_schedule: String,
    /// The simulated account's starting balances.
    pub balances: Vec<Money>,
    pub stp: SelfTradePrevention,
}

impl Default for SimSection {
    fn default() -> Self {
        Self {
            fill_model: FillModel::TopOfBook,
            latency: Latency::default(),
            fee_schedule: String::new(),
            balances: Vec::new(),
            stp: SelfTradePrevention::None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VenueConfig {
    pub id: String,
    pub kind: String,
    pub endpoint: Endpoint,
    /// A reference such as `env:NAME`, never the secret itself.
    pub credentials: String,
    pub exchange_info: String,
    pub account_mode: AccountMode,
    pub oms: OmsKind,
    /// Initial margin = notional / leverage; 0: the instrument's `margin_init`.
    pub leverage: u32,
    pub sim: Option<SimSection>,
}

impl Default for VenueConfig {
    fn default() -> Self {
        Self {
            id: String::new(),
            kind: String::new(),
            endpoint: Endpoint::Prod,
            credentials: String::new(),
            exchange_info: String::new(),
            account_mode: AccountMode::OneWay,
            oms: OmsKind::Netting,
            leverage: 0,
            sim: None,
        }
    }
}

/// Strategy parameters are defined by the strategy, so the node only checks their types.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ParamScalar {
    Bool(bool),
    Int(i64),
    Str(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ParamValue {
    Scalar(ParamScalar),
    List(Vec<ParamScalar>),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Param {
    /// Nested tables flattened to dotted keys.
    pub key: String,
    pub value: ParamValue,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StrategyConfig {
    pub id: String,
    /// `py:<class>`, `rust:<registered name>` or `cpp:<registered name>`.
    pub impl_: String,
    pub instruments: Vec<InstrumentId>,
    /// Sorted by key.
    pub params: Vec<Param>,
}

impl StrategyConfig {
    /// The parameter `key`, if given.
    #[must_use]
    pub fn param(&self, key: &str) -> Option<&ParamValue> {
        self.params.iter().find(|p| p.key == key).map(|p| &p.value)
    }
    #[must_use]
    pub fn param_str(&self, key: &str) -> Option<&str> {
        match self.param(key)? {
            ParamValue::Scalar(ParamScalar::Str(s)) => Some(s),
            _ => None,
        }
    }
    #[must_use]
    pub fn param_int(&self, key: &str) -> Option<i64> {
        match self.param(key)? {
            ParamValue::Scalar(ParamScalar::Int(i)) => Some(*i),
            _ => None,
        }
    }
    /// The name after the `rust:` or `cpp:` prefix of a native strategy.
    #[must_use]
    pub fn native_name(&self) -> Option<&str> {
        self.impl_.strip_prefix("rust:").or_else(|| self.impl_.strip_prefix("cpp:"))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RiskSection {
    pub initial_state: TradingState,
    pub max_order_notional: Option<Money>,
    pub max_position_notional: Option<Money>,
    pub daily_loss_limit: Option<Money>,
    pub daily_loss_halt: Option<Money>,
    pub max_drawdown: Option<Money>,
    pub price_band_bps: u32,
    pub max_open_orders: u32,
    pub orders_per_10s: u32,
    pub orders_per_minute: u32,
    pub margin_ratio_bps: u32,
    pub check_margin: bool,
    /// `env = "live"`; 0: off, else at least 10000.
    pub countdown_cancel_all_ms: u32,
    pub on_strategy_error: OnStrategyError,
}

impl Default for RiskSection {
    fn default() -> Self {
        Self {
            initial_state: TradingState::Active,
            max_order_notional: None,
            max_position_notional: None,
            daily_loss_limit: None,
            daily_loss_halt: None,
            max_drawdown: None,
            price_band_bps: 0,
            max_open_orders: 0,
            orders_per_10s: 250,
            orders_per_minute: 1000,
            margin_ratio_bps: 8000,
            check_margin: true,
            countdown_cancel_all_ms: 120_000,
            on_strategy_error: OnStrategyError::HaltStrategy,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PythonSection {
    pub callback_budget_us: u64,
    pub overrun_limit: u64,
    pub idle_hook_ms: u64,
}

impl Default for PythonSection {
    fn default() -> Self {
        Self { callback_budget_us: 2000, overrun_limit: 50, idle_hook_ms: 100 }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PersistenceSection {
    pub mode: PersistenceMode,
    pub dir: String,
    pub snapshot_every: u64,
    pub sync_every_ms: u64,
    pub raw_frames: RawFrames,
    pub resume: bool,
    pub truncate: bool,
}

impl Default for PersistenceSection {
    fn default() -> Self {
        Self {
            mode: PersistenceMode::Async,
            dir: "runs/{node_id}/{run_id}".to_string(),
            snapshot_every: 1_000_000,
            sync_every_ms: 100,
            raw_frames: RawFrames::On,
            resume: false,
            truncate: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TelemetrySection {
    pub prometheus: String,
    pub jsonl: bool,
}

impl Default for TelemetrySection {
    fn default() -> Self {
        Self { prometheus: String::new(), jsonl: true }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AdminSection {
    pub socket: String,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ThreadsSection {
    pub busy_poll: bool,
    pub core_cpu: Option<u32>,
    pub market_cpu: Option<u32>,
    pub venue_cpu: Option<u32>,
    pub numa_node: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NetworkSection {
    pub market_idle_timeout_ms: u32,
    pub venue_idle_timeout_ms: u32,
}

impl Default for NetworkSection {
    fn default() -> Self {
        Self { market_idle_timeout_ms: 30_000, venue_idle_timeout_ms: 60_000 }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NodeConfig {
    pub node: NodeSection,
    pub data: DataSection,
    pub venues: Vec<VenueConfig>,
    pub strategies: Vec<StrategyConfig>,
    pub risk: RiskSection,
    pub python: PythonSection,
    pub persistence: PersistenceSection,
    pub telemetry: TelemetrySection,
    pub admin: AdminSection,
    pub threads: ThreadsSection,
    pub network: NetworkSection,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfigError {
    /// E.g. `venues[0].oms`; empty for errors about the whole document.
    pub path: String,
    /// 1-based line in the source text, 0 when the value came from `--set`.
    pub line: u32,
    pub message: String,
}

/// Command-line overrides. `env` is applied first, then each `a.b.c=value` in order. A path segment
/// selects a table key, an array element by index, or the element of an array of tables whose `id`
/// equals the segment (`strategies.mm-001.params.size=0.020`). The value is read as TOML when it
/// parses as a string, integer, boolean, array or inline table; otherwise (bare words, decimals,
/// timestamps) as the text itself.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConfigOverrides {
    pub env: Option<String>,
    pub sets: Vec<String>,
}

// ---- enum names --------------------------------------------------------------------------------

const ENV_NAMES: &[(&str, Env)] =
    &[("backtest", Env::Backtest), ("sandbox", Env::Sandbox), ("live", Env::Live)];
const ACCOUNT_MODE_NAMES: &[(&str, AccountMode)] =
    &[("one_way", AccountMode::OneWay), ("hedge", AccountMode::Hedge)];
const OMS_NAMES: &[(&str, OmsKind)] =
    &[("netting", OmsKind::Netting), ("hedging", OmsKind::Hedging)];
const ENDPOINT_NAMES: &[(&str, Endpoint)] =
    &[("prod", Endpoint::Prod), ("testnet", Endpoint::Testnet)];
const FILL_MODEL_NAMES: &[(&str, FillModel)] =
    &[("top_of_book", FillModel::TopOfBook), ("queue_position", FillModel::QueuePosition)];
const STP_NAMES: &[(&str, SelfTradePrevention)] = &[
    ("none", SelfTradePrevention::None),
    ("expire_taker", SelfTradePrevention::ExpireTaker),
    ("expire_maker", SelfTradePrevention::ExpireMaker),
    ("expire_both", SelfTradePrevention::ExpireBoth),
];
const CODEC_NAMES: &[(&str, Codec)] = &[("json", Codec::Json), ("sbe", Codec::Sbe)];
const ON_ERROR_NAMES: &[(&str, OnStrategyError)] = &[
    ("halt_strategy", OnStrategyError::HaltStrategy),
    ("halt_node", OnStrategyError::HaltNode),
    ("ignore", OnStrategyError::Ignore),
];
const PERSISTENCE_NAMES: &[(&str, PersistenceMode)] = &[
    ("none", PersistenceMode::None),
    ("async", PersistenceMode::Async),
    ("barrier", PersistenceMode::Barrier),
];
const TRADING_STATE_NAMES: &[(&str, TradingState)] = &[
    ("active", TradingState::Active),
    ("reducing", TradingState::Reducing),
    ("halted", TradingState::Halted),
];
const SHUTDOWN_NAMES: &[(&str, StopMode)] = &[
    ("cancel_all_then_exit", StopMode::CancelAllThenExit),
    ("exit_keep_orders", StopMode::LeaveOrders),
    ("kill_switch", StopMode::KillSwitch),
];

fn name_of<E: PartialEq + Copy>(names: &[(&'static str, E)], value: E) -> &'static str {
    names.iter().find(|(_, v)| *v == value).map_or("?", |(n, _)| n)
}

fn choices<E>(names: &[(&str, E)]) -> String {
    names.iter().map(|(n, _)| format!("\"{n}\"")).collect::<Vec<_>>().join(", ")
}

pub fn env_name(env: Env) -> &'static str {
    name_of(ENV_NAMES, env)
}

// ---- reading -----------------------------------------------------------------------------------

struct Errors<'t> {
    text: &'t str,
    items: Vec<ConfigError>,
}

impl Errors<'_> {
    fn line_of(&self, span: Option<Span>) -> u32 {
        match span {
            Some(r) => 1 + self.text[..r.start.min(self.text.len())].matches('\n').count() as u32,
            None => 0,
        }
    }
    fn add(&mut self, path: &str, span: Option<Span>, message: impl Into<String>) {
        let line = self.line_of(span);
        self.items.push(ConfigError { path: path.to_string(), line, message: message.into() });
    }
}

fn type_name(item: &Item) -> &'static str {
    match item {
        Item::None => "nothing",
        Item::Value(v) => match v {
            Value::String(_) => "a string",
            Value::Integer(_) => "an integer",
            Value::Float(_) => "a floating-point number",
            Value::Boolean(_) => "a boolean",
            Value::Datetime(_) => "a date-time",
            Value::Array(_) => "an array",
            Value::InlineTable(_) => "a table",
        },
        Item::Table(_) => "a table",
        Item::ArrayOfTables(_) => "an array of tables",
    }
}

fn type_error(expected: &str, item: &Item) -> String {
    if item.is_float() {
        return "floating-point values are not allowed; write decimals as strings (\"0.010\")"
            .to_string();
    }
    format!("expected {expected}, found {}", type_name(item))
}

fn join_path(prefix: &str, key: &str) -> String {
    if prefix.is_empty() {
        key.to_string()
    } else {
        format!("{prefix}.{key}")
    }
}

fn index_path(prefix: &str, i: usize) -> String {
    format!("{prefix}[{i}]")
}

/// A table of the document with the span it was parsed from (`TableLike` has no span).
#[derive(Clone)]
struct Section<'a> {
    table: &'a dyn TableLike,
    span: Option<Span>,
}

type Span = std::ops::Range<usize>;

/// The tables of an item that is an array of tables (`[[x]]`) or an array of inline tables.
fn tables_of(item: &Item) -> Vec<Section<'_>> {
    let mut out: Vec<Section<'_>> = Vec::new();
    match item {
        Item::ArrayOfTables(a) => {
            out.extend(a.iter().map(|t| Section { table: t as &dyn TableLike, span: t.span() }));
        }
        Item::Value(Value::Array(a)) => {
            for v in a {
                if let Value::InlineTable(t) = v {
                    out.push(Section { table: t as &dyn TableLike, span: v.span() });
                }
            }
        }
        _ => {}
    }
    out
}

/// Reads the keys of one table, remembering which were consumed so the rest are reported.
struct TableReader<'a, 'e, 't> {
    table: &'a dyn TableLike,
    span: Option<Span>,
    path: String,
    used: BTreeSet<String>,
    errors: &'e mut Errors<'t>,
}

impl<'a, 'e, 't> TableReader<'a, 'e, 't> {
    fn new(section: Section<'a>, path: &str, errors: &'e mut Errors<'t>) -> Self {
        Self {
            table: section.table,
            span: section.span,
            path: path.to_string(),
            used: BTreeSet::new(),
            errors,
        }
    }

    fn path_of(&self, key: &str) -> String {
        join_path(&self.path, key)
    }

    fn take(&mut self, key: &str) -> Option<&'a Item> {
        self.used.insert(key.to_string());
        self.table.get(key).filter(|i| !i.is_none())
    }

    fn require(&mut self, key: &str) {
        if self.table.get(key).is_none_or(Item::is_none) {
            let span = self.span.clone();
            self.errors.add(&self.path_of(key), span, "missing required key");
        }
    }

    fn string(&mut self, key: &str, out: &mut String) {
        let Some(item) = self.take(key) else { return };
        if let Some(s) = item.as_str() {
            *out = s.to_string();
        } else {
            let m = type_error("a string", item);
            self.errors.add(&self.path_of(key), item.span(), m);
        }
    }

    /// An integer in `[minimum, maximum]`.
    fn unsigned(&mut self, key: &str, minimum: u64, maximum: u64) -> Option<u64> {
        let item = self.take(key)?;
        let Some(v) = item.as_integer() else {
            let m = type_error("an integer", item);
            self.errors.add(&self.path_of(key), item.span(), m);
            return None;
        };
        if v < 0 || (v as u64) < minimum || (v as u64) > maximum {
            self.errors.add(
                &self.path_of(key),
                item.span(),
                format!("must be between {minimum} and {maximum}"),
            );
            return None;
        }
        Some(v as u64)
    }

    fn u32(&mut self, key: &str, out: &mut u32) {
        if let Some(v) = self.unsigned(key, 0, u64::from(u32::MAX)) {
            *out = v as u32;
        }
    }
    fn u64(&mut self, key: &str, out: &mut u64, minimum: u64) {
        if let Some(v) = self.unsigned(key, minimum, u64::MAX) {
            *out = v;
        }
    }

    fn boolean(&mut self, key: &str, out: &mut bool) {
        let Some(item) = self.take(key) else { return };
        if let Some(b) = item.as_bool() {
            *out = b;
        } else {
            let m = type_error("a boolean", item);
            self.errors.add(&self.path_of(key), item.span(), m);
        }
    }

    fn enumeration<E: Copy>(&mut self, key: &str, out: &mut E, names: &[(&'static str, E)]) {
        let Some(item) = self.take(key) else { return };
        let Some(s) = item.as_str() else {
            let m = type_error("a string", item);
            self.errors.add(&self.path_of(key), item.span(), m);
            return;
        };
        if let Some((_, v)) = names.iter().find(|(n, _)| *n == s) {
            *out = *v;
        } else {
            let m = format!("must be one of {}", choices(names));
            self.errors.add(&self.path_of(key), item.span(), m);
        }
    }

    fn array(&mut self, key: &str) -> Option<&'a toml_edit::Array> {
        let item = self.take(key)?;
        let array = item.as_array();
        if array.is_none() {
            let m = type_error("an array", item);
            self.errors.add(&self.path_of(key), item.span(), m);
        }
        array
    }

    fn strings(&mut self, key: &str, out: &mut Vec<String>) {
        let Some(array) = self.array(key) else { return };
        out.clear();
        for (i, v) in array.iter().enumerate() {
            if let Some(s) = v.as_str() {
                out.push(s.to_string());
            } else {
                let m = type_error("a string", &Item::Value(v.clone()));
                self.errors.add(&index_path(&self.path_of(key), i), v.span(), m);
            }
        }
    }

    fn instrument_ids(&mut self, key: &str, out: &mut Vec<InstrumentId>) {
        let mut texts = Vec::new();
        self.strings(key, &mut texts);
        out.clear();
        for (i, text) in texts.iter().enumerate() {
            if let Ok(id) = InstrumentId::parse(text) {
                out.push(id);
            } else {
                let span = self.table.get(key).and_then(Item::span);
                self.errors.add(
                    &index_path(&self.path_of(key), i),
                    span,
                    format!("\"{text}\" is not an instrument id (SYMBOL.VENUE)"),
                );
            }
        }
    }

    fn money(&mut self, key: &str, out: &mut Option<Money>) {
        let Some(item) = self.take(key) else { return };
        let Some(s) = item.as_str() else {
            let m = type_error("a string", item);
            self.errors.add(&self.path_of(key), item.span(), m);
            return;
        };
        match Money::parse(s) {
            Ok(m) => *out = Some(m),
            Err(_) => self.errors.add(
                &self.path_of(key),
                item.span(),
                format!("\"{s}\" is not an amount (\"50000 USDT\")"),
            ),
        }
    }

    fn moneys(&mut self, key: &str, out: &mut Vec<Money>) {
        let mut texts = Vec::new();
        self.strings(key, &mut texts);
        out.clear();
        for (i, text) in texts.iter().enumerate() {
            if let Ok(m) = Money::parse(text) {
                out.push(m);
            } else {
                let span = self.table.get(key).and_then(Item::span);
                self.errors.add(
                    &index_path(&self.path_of(key), i),
                    span,
                    format!("\"{text}\" is not an amount (\"10000 USDT\")"),
                );
            }
        }
    }

    /// A TOML date-time, or an RFC 3339 string.
    fn timestamp(&mut self, key: &str, out: &mut UnixNanos) {
        let Some(item) = self.take(key) else { return };
        let text = match item.as_value() {
            Some(Value::Datetime(d)) => d.value().to_string(),
            Some(Value::String(s)) => s.value().clone(),
            _ => {
                let m = type_error("a date-time", item);
                self.errors.add(&self.path_of(key), item.span(), m);
                return;
            }
        };
        match parse_rfc3339(&text) {
            Ok(t) => *out = t,
            Err(_) => self.errors.add(
                &self.path_of(key),
                item.span(),
                format!("\"{text}\" is not an RFC 3339 timestamp with an offset"),
            ),
        }
    }

    fn table(&mut self, key: &str) -> Option<Section<'a>> {
        let item = self.take(key)?;
        if let Some(t) = item.as_table_like() {
            Some(Section { table: t, span: item.span() })
        } else {
            let m = type_error("a table", item);
            self.errors.add(&self.path_of(key), item.span(), m);
            None
        }
    }

    /// The tables of `key` (`[[key]]`), each with its indexed path.
    fn each_table(&mut self, key: &str) -> Vec<(Section<'a>, String)> {
        let Some(item) = self.take(key) else { return Vec::new() };
        let tables = tables_of(item);
        if tables.is_empty()
            && !matches!(item, Item::ArrayOfTables(_) | Item::Value(Value::Array(_)))
        {
            let m = type_error("an array of tables", item);
            self.errors.add(&self.path_of(key), item.span(), m);
            return Vec::new();
        }
        let base = self.path_of(key);
        tables.into_iter().enumerate().map(|(i, t)| (t, index_path(&base, i))).collect()
    }

    /// Reports the keys nobody consumed.
    fn finish(self) {
        for (k, item) in self.table.iter() {
            if !self.used.contains(k) {
                let span = item.span().or_else(|| self.span.clone());
                self.errors.add(&self.path_of(k), span, "unknown key");
            }
        }
    }
}

/// Floats anywhere in the document are errors, found before the typed reads.
fn reject_floats(item: &Item, path: &str, errors: &mut Errors<'_>) {
    match item {
        Item::Value(v) => reject_floats_value(v, path, errors),
        Item::Table(t) => {
            for (k, i) in t {
                reject_floats(i, &join_path(path, k), errors);
            }
        }
        Item::ArrayOfTables(a) => {
            for (n, t) in a.iter().enumerate() {
                for (k, i) in t {
                    reject_floats(i, &join_path(&index_path(path, n), k), errors);
                }
            }
        }
        Item::None => {}
    }
}

fn reject_floats_value(v: &Value, path: &str, errors: &mut Errors<'_>) {
    match v {
        Value::Float(_) => errors.add(
            path,
            v.span(),
            "floating-point values are not allowed; write decimals as strings (\"0.010\")",
        ),
        Value::Array(a) => {
            for (i, x) in a.iter().enumerate() {
                reject_floats_value(x, &index_path(path, i), errors);
            }
        }
        Value::InlineTable(t) => {
            for (k, x) in t {
                reject_floats_value(x, &join_path(path, k), errors);
            }
        }
        _ => {}
    }
}

fn valid_node_tag(id: &str) -> bool {
    model::client_order_id::is_valid_tag(id)
}

fn read_capacity(t: Section<'_>, path: &str, errors: &mut Errors<'_>, out: &mut Capacity) {
    let mut r = TableReader::new(t, path, errors);
    if let Some(v) = r.unsigned("orders", 1, u64::from(u32::MAX)) {
        out.orders = v as u32;
    }
    if let Some(v) = r.unsigned("instruments", 1, u64::from(u32::MAX)) {
        out.instruments = v as u32;
    }
    if let Some(v) = r.unsigned("batch", 1, u64::from(u32::MAX)) {
        out.batch = v as u32;
    }
    if let Some(v) = r.unsigned("timers", 1, u64::from(u32::MAX)) {
        out.timers = v as u32;
    }
    if let Some(v) = r.unsigned("strategies", 1, u64::from(u16::MAX)) {
        out.strategies = v as u32;
    }
    r.finish();
}

fn read_node(root: &mut TableReader<'_, '_, '_>, n: &mut NodeSection) {
    let Some(t) = root.table("node") else {
        let span = root.span.clone();
        root.errors.add("node", span, "missing required table [node]");
        return;
    };
    let table = t.table;
    let mut r = TableReader::new(t, "node", root.errors);
    r.require("id");
    r.string("id", &mut n.id);
    r.enumeration("env", &mut n.env, ENV_NAMES);
    r.u64("seed", &mut n.seed, 0);
    r.boolean("strict_determinism", &mut n.strict_determinism);
    r.enumeration("shutdown", &mut n.shutdown, SHUTDOWN_NAMES);
    r.u32("shutdown_timeout_ms", &mut n.shutdown_timeout_ms);
    r.u32("market_data_stale_ms", &mut n.market_data_stale_ms);
    if let Some(cap) = r.table("capacity") {
        read_capacity(cap, "node.capacity", r.errors, &mut n.capacity);
    }
    r.finish();
    if !n.id.is_empty() && !valid_node_tag(&n.id) {
        let span = table.get("id").and_then(Item::span);
        root.errors.add(
            "node.id",
            span,
            "must be 1-8 ASCII letters or digits (it prefixes every client order id)",
        );
    }
}

fn read_data(root: &mut TableReader<'_, '_, '_>, d: &mut DataSection) {
    let Some(t) = root.table("data") else { return };
    let mut r = TableReader::new(t, "data", root.errors);
    r.string("catalog", &mut d.catalog);
    if let Some(range) = r.table("range") {
        let range_span = range.span.clone();
        let mut rr = TableReader::new(range, "data.range", r.errors);
        let mut value = TimeRange { start: UnixNanos::default(), end: UnixNanos::default() };
        rr.require("start");
        rr.require("end");
        rr.timestamp("start", &mut value.start);
        rr.timestamp("end", &mut value.end);
        rr.finish();
        if value.start >= value.end {
            r.errors.add("data.range", range_span, "start must be before end");
        }
        d.range = Some(value);
    }
    for (st, path) in r.each_table("streams") {
        let mut sr = TableReader::new(st, &path, r.errors);
        let mut stream = DataStream {
            venue: String::new(),
            instruments: Vec::new(),
            streams: Vec::new(),
            codec: Codec::Json,
        };
        sr.require("venue");
        sr.string("venue", &mut stream.venue);
        sr.instrument_ids("instruments", &mut stream.instruments);
        sr.strings("streams", &mut stream.streams);
        sr.enumeration("codec", &mut stream.codec, CODEC_NAMES);
        sr.finish();
        d.streams.push(stream);
    }
    r.finish();
}

fn read_sim(t: Section<'_>, path: &str, errors: &mut Errors<'_>, sim: &mut SimSection) {
    let mut r = TableReader::new(t, path, errors);
    r.enumeration("fill_model", &mut sim.fill_model, FILL_MODEL_NAMES);
    if let Some(lat) = r.table("latency") {
        let mut lr = TableReader::new(lat, &format!("{path}.latency"), r.errors);
        lr.u64("feed_ns", &mut sim.latency.feed_ns, 0);
        lr.u64("out_ns", &mut sim.latency.out_ns, 0);
        lr.u64("in_ns", &mut sim.latency.in_ns, 0);
        lr.u64("jitter_ns", &mut sim.latency.jitter_ns, 0);
        lr.finish();
    }
    if let Some(fee) = r.table("fee") {
        let mut fr = TableReader::new(fee, &format!("{path}.fee"), r.errors);
        fr.string("schedule", &mut sim.fee_schedule);
        fr.finish();
    }
    r.moneys("balances", &mut sim.balances);
    r.enumeration("stp", &mut sim.stp, STP_NAMES);
    r.finish();
}

fn read_venue(t: Section<'_>, path: &str, errors: &mut Errors<'_>, v: &mut VenueConfig) {
    let table = t.table;
    let mut r = TableReader::new(t, path, errors);
    r.require("id");
    r.require("kind");
    r.string("id", &mut v.id);
    r.string("kind", &mut v.kind);
    r.enumeration("endpoint", &mut v.endpoint, ENDPOINT_NAMES);
    r.string("credentials", &mut v.credentials);
    r.string("exchange_info", &mut v.exchange_info);
    r.enumeration("account_mode", &mut v.account_mode, ACCOUNT_MODE_NAMES);
    r.enumeration("oms", &mut v.oms, OMS_NAMES);
    r.u32("leverage", &mut v.leverage);
    if let Some(sim) = r.table("sim") {
        let mut s = SimSection::default();
        read_sim(sim, &format!("{path}.sim"), r.errors, &mut s);
        v.sim = Some(s);
    }
    r.finish();
    let consistent = matches!(
        (v.account_mode, v.oms),
        (AccountMode::OneWay, OmsKind::Netting) | (AccountMode::Hedge, OmsKind::Hedging)
    );
    if !consistent {
        let span = table.get("oms").and_then(Item::span);
        errors.add(
            &format!("{path}.oms"),
            span,
            format!(
                "must be {} when account_mode is {}",
                if v.account_mode == AccountMode::OneWay { "netting" } else { "hedging" },
                name_of(ACCOUNT_MODE_NAMES, v.account_mode)
            ),
        );
    }
    if !v.credentials.is_empty()
        && !v.credentials.starts_with("env:")
        && !v.credentials.starts_with("file:")
    {
        let span = table.get("credentials").and_then(Item::span);
        errors.add(
            &format!("{path}.credentials"),
            span,
            "must be a reference (\"env:NAME\" or \"file:PATH\"), never the secret itself",
        );
    }
}

fn scalar_of(v: &Value) -> Option<ParamScalar> {
    match v {
        Value::Boolean(b) => Some(ParamScalar::Bool(*b.value())),
        Value::Integer(i) => Some(ParamScalar::Int(*i.value())),
        Value::String(s) => Some(ParamScalar::Str(s.value().clone())),
        _ => None,
    }
}

fn flatten_params(
    t: &dyn TableLike,
    prefix: &str,
    path: &str,
    errors: &mut Errors<'_>,
    out: &mut Vec<Param>,
) {
    for (key, item) in t.iter() {
        let name = join_path(prefix, key);
        let node_path = join_path(path, key);
        if let Some(sub) = item.as_table_like() {
            flatten_params(sub, &name, &node_path, errors, out);
            continue;
        }
        let Some(value) = item.as_value() else {
            let m = type_error("a boolean, integer, string or array", item);
            errors.add(&node_path, item.span(), m);
            continue;
        };
        if let Some(s) = scalar_of(value) {
            out.push(Param { key: name, value: ParamValue::Scalar(s) });
        } else if let Some(arr) = value.as_array() {
            let mut items = Vec::new();
            for (i, x) in arr.iter().enumerate() {
                if let Some(s) = scalar_of(x) {
                    items.push(s);
                } else {
                    let m = type_error("a boolean, integer or string", &Item::Value(x.clone()));
                    errors.add(&index_path(&node_path, i), x.span(), m);
                }
            }
            out.push(Param { key: name, value: ParamValue::List(items) });
        } else {
            let m = type_error("a boolean, integer, string or array", item);
            errors.add(&node_path, item.span(), m);
        }
    }
}

fn read_strategy(t: Section<'_>, path: &str, errors: &mut Errors<'_>, s: &mut StrategyConfig) {
    let table = t.table;
    let mut r = TableReader::new(t, path, errors);
    r.require("id");
    r.require("impl");
    r.string("id", &mut s.id);
    r.string("impl", &mut s.impl_);
    r.instrument_ids("instruments", &mut s.instruments);
    if let Some(params) = r.table("params") {
        flatten_params(params.table, "", &format!("{path}.params"), r.errors, &mut s.params);
        s.params.sort_by(|a, b| a.key.cmp(&b.key));
    }
    r.finish();
    if !s.id.is_empty() && StrategyId::new(&s.id).is_err() {
        let span = table.get("id").and_then(Item::span);
        errors.add(&format!("{path}.id"), span, "must look like \"<name>-<tag>\", e.g. \"mm-001\"");
    }
    let py = s.impl_.strip_prefix("py:").is_some_and(|n| !n.is_empty());
    let native = s.native_name().is_some_and(|n| !n.is_empty());
    if !s.impl_.is_empty() && !py && !native {
        let span = table.get("impl").and_then(Item::span);
        errors.add(
            &format!("{path}.impl"),
            span,
            "must be \"py:<class>\", \"rust:<registered name>\" or \"cpp:<registered name>\"",
        );
    }
}

/// A shorter countdown would be renewed more often than its request weight allows for long.
const MIN_COUNTDOWN_MS: u32 = 10_000;

fn read_risk(root: &mut TableReader<'_, '_, '_>, risk: &mut RiskSection) {
    let Some(t) = root.table("risk") else { return };
    let table = t.table;
    let mut r = TableReader::new(t, "risk", root.errors);
    r.enumeration("initial_state", &mut risk.initial_state, TRADING_STATE_NAMES);
    r.money("max_order_notional", &mut risk.max_order_notional);
    r.money("max_position_notional", &mut risk.max_position_notional);
    r.money("daily_loss_limit", &mut risk.daily_loss_limit);
    r.money("daily_loss_halt", &mut risk.daily_loss_halt);
    r.money("max_drawdown", &mut risk.max_drawdown);
    r.u32("price_band_bps", &mut risk.price_band_bps);
    r.u32("max_open_orders", &mut risk.max_open_orders);
    r.u32("orders_per_10s", &mut risk.orders_per_10s);
    r.u32("orders_per_minute", &mut risk.orders_per_minute);
    r.u32("margin_ratio_bps", &mut risk.margin_ratio_bps);
    r.boolean("check_margin", &mut risk.check_margin);
    r.u32("countdown_cancel_all_ms", &mut risk.countdown_cancel_all_ms);
    if risk.countdown_cancel_all_ms != 0 && risk.countdown_cancel_all_ms < MIN_COUNTDOWN_MS {
        let span = table.get("countdown_cancel_all_ms").and_then(Item::span);
        r.errors.add(
            "risk.countdown_cancel_all_ms",
            span,
            format!("must be 0 (off) or at least {MIN_COUNTDOWN_MS} (it is renewed every quarter of it)"),
        );
    }
    r.enumeration("on_strategy_error", &mut risk.on_strategy_error, ON_ERROR_NAMES);
    r.finish();
}

fn read_python(root: &mut TableReader<'_, '_, '_>, py: &mut PythonSection) {
    let Some(t) = root.table("python") else { return };
    let mut r = TableReader::new(t, "python", root.errors);
    r.u64("callback_budget_us", &mut py.callback_budget_us, 1);
    r.u64("overrun_limit", &mut py.overrun_limit, 1);
    r.u64("idle_hook_ms", &mut py.idle_hook_ms, 0);
    r.finish();
}

fn read_raw_frames(r: &mut TableReader<'_, '_, '_>, out: &mut RawFrames) {
    let Some(item) = r.take("raw_frames") else { return };
    match (item.as_bool(), item.as_str()) {
        (Some(true), _) => *out = RawFrames::On,
        (Some(false), _) => *out = RawFrames::Off,
        (None, Some("sampled")) => *out = RawFrames::Sampled,
        _ => {
            let p = r.path_of("raw_frames");
            r.errors.add(&p, item.span(), "must be true, false or \"sampled\"");
        }
    }
}

/// `host:port` with a port from 0 to 65535 (an IPv6 host in brackets).
fn valid_listen(text: &str) -> bool {
    let Some(colon) = text.rfind(':') else { return false };
    if colon == 0 || colon + 1 >= text.len() {
        return false;
    }
    text[colon + 1..].parse::<u32>().is_ok_and(|p| p <= 65_535)
}

fn cpu_number(r: &mut TableReader<'_, '_, '_>, key: &str, out: &mut Option<u32>) {
    let Some(item) = r.take(key) else { return };
    match item.as_integer() {
        Some(i) if (0..1024).contains(&i) => *out = Some(i as u32),
        _ => {
            let p = r.path_of(key);
            r.errors.add(&p, item.span(), "must be an integer from 0 to 1023");
        }
    }
}

fn read_threads(r: &mut TableReader<'_, '_, '_>, out: &mut ThreadsSection) {
    r.boolean("busy_poll", &mut out.busy_poll);
    cpu_number(r, "core_cpu", &mut out.core_cpu);
    cpu_number(r, "market_cpu", &mut out.market_cpu);
    cpu_number(r, "venue_cpu", &mut out.venue_cpu);
    cpu_number(r, "numa_node", &mut out.numa_node);
    let span = r.span.clone();
    if out.core_cpu.is_some() && out.market_cpu == out.core_cpu {
        r.errors.add("threads.market_cpu", span.clone(), "is the core thread's CPU");
    }
    if out.core_cpu.is_some() && out.venue_cpu == out.core_cpu {
        r.errors.add("threads.venue_cpu", span.clone(), "is the core thread's CPU");
    }
    if out.busy_poll && out.market_cpu.is_some() && out.venue_cpu == out.market_cpu {
        r.errors.add(
            "threads.venue_cpu",
            span,
            "is the market data thread's CPU; with busy_poll each needs its own",
        );
    }
}

/// 0 (no deadline) or at least a second.
fn idle_timeout_ms(r: &mut TableReader<'_, '_, '_>, key: &str, out: &mut u32) {
    r.u32(key, out);
    if *out != 0 && *out < 1000 {
        let (p, span) = (r.path_of(key), r.span.clone());
        r.errors.add(&p, span, "must be 0 (off) or at least 1000");
        *out = 0;
    }
}

fn read_operations(root: &mut TableReader<'_, '_, '_>, c: &mut NodeConfig) {
    if let Some(t) = root.table("persistence") {
        let mut r = TableReader::new(t, "persistence", root.errors);
        r.enumeration("mode", &mut c.persistence.mode, PERSISTENCE_NAMES);
        r.string("dir", &mut c.persistence.dir);
        r.u64("snapshot_every", &mut c.persistence.snapshot_every, 1);
        r.u64("sync_every_ms", &mut c.persistence.sync_every_ms, 0);
        read_raw_frames(&mut r, &mut c.persistence.raw_frames);
        r.boolean("resume", &mut c.persistence.resume);
        r.boolean("truncate", &mut c.persistence.truncate);
        r.finish();
    }
    if let Some(t) = root.table("telemetry") {
        let mut r = TableReader::new(t, "telemetry", root.errors);
        r.string("prometheus", &mut c.telemetry.prometheus);
        r.boolean("jsonl", &mut c.telemetry.jsonl);
        if !c.telemetry.prometheus.is_empty() && !valid_listen(&c.telemetry.prometheus) {
            r.errors.add(
                "telemetry.prometheus",
                r.span.clone(),
                "expected host:port (such as \"0.0.0.0:9100\" or \"127.0.0.1:0\")",
            );
        }
        r.finish();
    }
    if let Some(t) = root.table("admin") {
        let mut r = TableReader::new(t, "admin", root.errors);
        r.string("socket", &mut c.admin.socket);
        r.finish();
    }
    if let Some(t) = root.table("threads") {
        let mut r = TableReader::new(t, "threads", root.errors);
        read_threads(&mut r, &mut c.threads);
        r.finish();
    }
    if let Some(t) = root.table("network") {
        let mut r = TableReader::new(t, "network", root.errors);
        idle_timeout_ms(&mut r, "market_idle_timeout_ms", &mut c.network.market_idle_timeout_ms);
        idle_timeout_ms(&mut r, "venue_idle_timeout_ms", &mut c.network.venue_idle_timeout_ms);
        r.finish();
    }
}

/// Checks that span sections: unique ids, references between sections, capacities.
fn cross_check(c: &NodeConfig, errors: &mut Errors<'_>) {
    let mut venue_ids = BTreeSet::new();
    for (i, v) in c.venues.iter().enumerate() {
        if !v.id.is_empty() && !venue_ids.insert(v.id.clone()) {
            errors.add(
                &format!("{}.id", index_path("venues", i)),
                None,
                format!("duplicate venue id {}", v.id),
            );
        }
    }
    let mut strategy_ids = BTreeSet::new();
    for (i, s) in c.strategies.iter().enumerate() {
        if !s.id.is_empty() && !strategy_ids.insert(s.id.clone()) {
            errors.add(
                &format!("{}.id", index_path("strategies", i)),
                None,
                format!("duplicate strategy id {}", s.id),
            );
        }
    }
    if c.strategies.len() > c.node.capacity.strategies as usize {
        errors.add(
            "strategies",
            None,
            format!(
                "{} strategies exceed node.capacity.strategies ({})",
                c.strategies.len(),
                c.node.capacity.strategies
            ),
        );
    }
    for (i, s) in c.data.streams.iter().enumerate() {
        if !s.venue.is_empty() && !venue_ids.contains(&s.venue) {
            errors.add(
                &format!("{}.venue", index_path("data.streams", i)),
                None,
                format!("no [[venues]] entry with id {}", s.venue),
            );
        }
    }
}

// ---- overrides ---------------------------------------------------------------------------------

fn is_index(segment: &str) -> bool {
    !segment.is_empty() && segment.bytes().all(|b| b.is_ascii_digit())
}

/// The value of an override: a TOML value when the text parses as a string, integer, boolean,
/// array or inline table; the text itself otherwise (bare words, decimals, timestamps).
fn override_value(text: &str) -> Item {
    if let Ok(doc) = format!("v = {text}").parse::<DocumentMut>() {
        if let Some(Item::Value(v)) = doc.get("v") {
            let usable = matches!(
                v,
                Value::String(_)
                    | Value::Integer(_)
                    | Value::Boolean(_)
                    | Value::Array(_)
                    | Value::InlineTable(_)
            );
            if usable {
                return Item::Value(v.clone());
            }
        }
    }
    toml_edit::value(text)
}

/// Element `segment` of an array of tables: by index, or the table whose `id` equals it.
fn element_index(tables: &[&dyn TableLike], segment: &str) -> Option<usize> {
    if is_index(segment) {
        let i: usize = segment.parse().ok()?;
        return (i < tables.len()).then_some(i);
    }
    tables.iter().position(|t| t.get("id").and_then(Item::as_str) == Some(segment))
}

fn set_in_table(
    t: &mut dyn TableLike,
    segments: &[&str],
    value: Item,
) -> std::result::Result<(), String> {
    let (seg, rest) = segments.split_first().ok_or_else(|| "empty path".to_string())?;
    if rest.is_empty() {
        t.insert(seg, value);
        return Ok(());
    }
    if t.get(seg).is_none_or(Item::is_none) {
        t.insert(seg, Item::Table(toml_edit::Table::new()));
    }
    let child = t.get_mut(seg).ok_or_else(|| format!("no element '{seg}' on this path"))?;
    set_in_item(child, rest, value)
}

fn set_in_item(item: &mut Item, segments: &[&str], value: Item) -> std::result::Result<(), String> {
    let (seg, rest) = segments.split_first().ok_or_else(|| "empty path".to_string())?;
    if let Some(t) = item.as_table_like_mut() {
        return set_in_table(t, segments, value);
    }
    match item {
        Item::ArrayOfTables(a) => {
            let index = {
                let tables: Vec<&dyn TableLike> = a.iter().map(|t| t as &dyn TableLike).collect();
                element_index(&tables, seg)
            };
            let i = index.ok_or_else(|| format!("no element '{seg}' on this path"))?;
            if rest.is_empty() {
                return Err(format!("the parent of '{seg}' is not a table"));
            }
            let t = a.get_mut(i).ok_or_else(|| "index out of range".to_string())?;
            set_in_table(t as &mut dyn TableLike, rest, value)
        }
        Item::Value(Value::Array(a)) => {
            let index = {
                let tables: Vec<&dyn TableLike> = a
                    .iter()
                    .filter_map(|v| v.as_inline_table().map(|t| t as &dyn TableLike))
                    .collect();
                if tables.len() == a.len() {
                    element_index(&tables, seg)
                } else if is_index(seg) {
                    seg.parse::<usize>().ok().filter(|&i| i < a.len())
                } else {
                    None
                }
            };
            let i = index.ok_or_else(|| format!("no element '{seg}' on this path"))?;
            if rest.is_empty() {
                let Item::Value(v) = value else {
                    return Err("--set: a table cannot replace an array element".to_string());
                };
                a.replace(i, v);
                return Ok(());
            }
            let elem = a.get_mut(i).ok_or_else(|| "index out of range".to_string())?;
            match elem {
                Value::InlineTable(t) => set_in_table(t as &mut dyn TableLike, rest, value),
                _ => Err(format!("the parent of '{}' is not a table", rest[0])),
            }
        }
        _ => Err(format!("the parent of '{seg}' is not a table")),
    }
}

fn apply_set(root: &mut DocumentMut, assignment: &str, errors: &mut Errors<'_>) {
    let Some((path, text)) = assignment.split_once('=') else {
        errors.add(assignment, None, "--set expects path=value");
        return;
    };
    if path.is_empty() {
        errors.add(assignment, None, "--set expects path=value");
        return;
    }
    let segments: Vec<&str> = path.split('.').collect();
    let value = override_value(text);
    if let Err(m) = set_in_table(root.as_table_mut() as &mut dyn TableLike, &segments, value) {
        errors.add(path, None, format!("--set: {m}"));
    }
}

// ---- parse -------------------------------------------------------------------------------------

/// Parses `text` and applies `overrides`. Every problem is reported, in source order, with the
/// ones from overrides and cross-section checks last.
pub fn parse_config(
    text: &str,
    overrides: &ConfigOverrides,
) -> std::result::Result<NodeConfig, Vec<ConfigError>> {
    let mut errors = Errors { text, items: Vec::new() };
    let mut root: DocumentMut = match text.parse() {
        Ok(doc) => doc,
        Err(e) => {
            let line = errors.line_of(e.span());
            return Err(vec![ConfigError {
                path: String::new(),
                line,
                message: e.message().to_string(),
            }]);
        }
    };
    if let Some(env) = &overrides.env {
        apply_set(&mut root, &format!("node.env={env}"), &mut errors);
    }
    for set in &overrides.sets {
        apply_set(&mut root, set, &mut errors);
    }
    for (k, item) in root.as_table() {
        reject_floats(item, k, &mut errors);
    }
    let mut config = NodeConfig::default();
    {
        let table: &dyn TableLike = root.as_table();
        let section = Section { table, span: root.as_table().span() };
        let mut r = TableReader::new(section, "", &mut errors);
        read_node(&mut r, &mut config.node);
        read_data(&mut r, &mut config.data);
        for (t, path) in r.each_table("venues") {
            let mut v = VenueConfig::default();
            read_venue(t, &path, r.errors, &mut v);
            config.venues.push(v);
        }
        for (t, path) in r.each_table("strategies") {
            let mut s = StrategyConfig::default();
            read_strategy(t, &path, r.errors, &mut s);
            config.strategies.push(s);
        }
        read_risk(&mut r, &mut config.risk);
        read_python(&mut r, &mut config.python);
        read_operations(&mut r, &mut config);
        r.finish();
    }
    cross_check(&config, &mut errors);
    if errors.items.is_empty() {
        return Ok(config);
    }
    let mut items = errors.items;
    items.sort_by_key(|e| if e.line == 0 { u32::MAX } else { e.line });
    Err(items)
}

/// `source:line: path: message` per error, one per line.
#[must_use]
pub fn format_errors(source: &str, errors: &[ConfigError]) -> String {
    let mut out = String::new();
    for e in errors {
        out.push_str(source);
        if e.line > 0 {
            let _ = write!(out, ":{}", e.line);
        }
        out.push_str(": ");
        if !e.path.is_empty() {
            out.push_str(&e.path);
            out.push_str(": ");
        }
        out.push_str(&e.message);
        out.push('\n');
    }
    out
}

/// Reads and parses a file; a file that cannot be read is one error.
pub fn load_config(
    path: &std::path::Path,
    overrides: &ConfigOverrides,
) -> std::result::Result<(NodeConfig, String), Vec<ConfigError>> {
    let text = sys::read_text(path).map_err(|_| {
        vec![ConfigError {
            path: String::new(),
            line: 0,
            message: format!("cannot read {}", path.display()),
        }]
    })?;
    parse_config(&text, overrides).map(|c| (c, text))
}

// ---- canonical text ----------------------------------------------------------------------------

fn quote(out: &mut String, text: &str) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' | '\\' => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u00{:02x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

#[derive(Default)]
struct Canon {
    out: String,
}

impl Canon {
    fn raw(&mut self, path: &str, value: &str) {
        let _ = writeln!(self.out, "{path} = {value}");
    }
    fn str(&mut self, path: &str, value: &str) {
        let mut v = String::new();
        quote(&mut v, value);
        self.raw(path, &v);
    }
    fn num(&mut self, path: &str, value: u64) {
        self.raw(path, &value.to_string());
    }
    fn flag(&mut self, path: &str, value: bool) {
        self.raw(path, if value { "true" } else { "false" });
    }
    fn cpu(&mut self, path: &str, value: Option<u32>) {
        self.raw(path, &value.map_or("none".to_string(), |v| v.to_string()));
    }
    fn strs(&mut self, path: &str, items: &[String]) {
        let mut v = String::from("[");
        for (i, s) in items.iter().enumerate() {
            if i > 0 {
                v.push_str(", ");
            }
            quote(&mut v, s);
        }
        v.push(']');
        self.raw(path, &v);
    }
    fn ids(&mut self, path: &str, ids: &[InstrumentId]) {
        let texts: Vec<String> = ids.iter().map(ToString::to_string).collect();
        self.strs(path, &texts);
    }
    fn money(&mut self, path: &str, value: Option<Money>) {
        match value {
            None => self.raw(path, "none"),
            Some(m) => self.str(path, &m.to_string()),
        }
    }
    fn time(&mut self, path: &str, t: UnixNanos) {
        let mut buf = [0u8; RFC3339_MAX_LENGTH];
        let n = format_rfc3339(t, &mut buf).unwrap_or(0);
        self.str(path, core::str::from_utf8(&buf[..n]).unwrap_or(""));
    }
}

fn scalar_text(out: &mut String, v: &ParamScalar) {
    match v {
        ParamScalar::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        ParamScalar::Int(i) => out.push_str(&i.to_string()),
        ParamScalar::Str(s) => quote(out, s),
    }
}

fn param_text(value: &ParamValue) -> String {
    let mut out = String::new();
    match value {
        ParamValue::Scalar(s) => scalar_text(&mut out, s),
        ParamValue::List(items) => {
            out.push('[');
            for (i, s) in items.iter().enumerate() {
                if i > 0 {
                    out.push_str(", ");
                }
                scalar_text(&mut out, s);
            }
            out.push(']');
        }
    }
    out
}

fn canon_venue(c: &mut Canon, v: &VenueConfig, p: &str) {
    c.str(&format!("{p}.id"), &v.id);
    c.str(&format!("{p}.kind"), &v.kind);
    c.str(&format!("{p}.account_mode"), name_of(ACCOUNT_MODE_NAMES, v.account_mode));
    c.str(&format!("{p}.oms"), name_of(OMS_NAMES, v.oms));
    c.num(&format!("{p}.leverage"), u64::from(v.leverage));
    let Some(sim) = &v.sim else {
        c.raw(&format!("{p}.sim"), "none");
        return;
    };
    c.str(&format!("{p}.sim.fill_model"), name_of(FILL_MODEL_NAMES, sim.fill_model));
    c.num(&format!("{p}.sim.latency.feed_ns"), sim.latency.feed_ns);
    c.num(&format!("{p}.sim.latency.out_ns"), sim.latency.out_ns);
    c.num(&format!("{p}.sim.latency.in_ns"), sim.latency.in_ns);
    c.num(&format!("{p}.sim.latency.jitter_ns"), sim.latency.jitter_ns);
    c.str(&format!("{p}.sim.fee.schedule"), &sim.fee_schedule);
    let balances: Vec<String> = sim.balances.iter().map(ToString::to_string).collect();
    c.strs(&format!("{p}.sim.balances"), &balances);
    c.str(&format!("{p}.sim.stp"), name_of(STP_NAMES, sim.stp));
}

fn canon_strategy(c: &mut Canon, s: &StrategyConfig, p: &str) {
    c.str(&format!("{p}.id"), &s.id);
    c.str(&format!("{p}.impl"), &s.impl_);
    c.ids(&format!("{p}.instruments"), &s.instruments);
    for param in &s.params {
        c.raw(&format!("{p}.params.{}", param.key), &param_text(&param.value));
    }
}

/// The hashed part of the canonical form.
#[must_use]
pub fn canonical_hashed_text(config: &NodeConfig) -> String {
    let mut c = Canon::default();
    let n = &config.node;
    c.str("node.id", &n.id);
    c.num("node.seed", n.seed);
    c.flag("node.strict_determinism", n.strict_determinism);
    c.num("node.market_data_stale_ms", u64::from(n.market_data_stale_ms));
    c.num("node.capacity.orders", u64::from(n.capacity.orders));
    c.num("node.capacity.instruments", u64::from(n.capacity.instruments));
    c.num("node.capacity.batch", u64::from(n.capacity.batch));
    c.num("node.capacity.timers", u64::from(n.capacity.timers));
    c.num("node.capacity.strategies", u64::from(n.capacity.strategies));
    for (i, v) in config.venues.iter().enumerate() {
        canon_venue(&mut c, v, &index_path("venues", i));
    }
    for (i, s) in config.strategies.iter().enumerate() {
        canon_strategy(&mut c, s, &index_path("strategies", i));
    }
    let risk = &config.risk;
    c.str("risk.initial_state", name_of(TRADING_STATE_NAMES, risk.initial_state));
    c.money("risk.max_order_notional", risk.max_order_notional);
    c.money("risk.max_position_notional", risk.max_position_notional);
    c.money("risk.daily_loss_limit", risk.daily_loss_limit);
    c.money("risk.daily_loss_halt", risk.daily_loss_halt);
    c.money("risk.max_drawdown", risk.max_drawdown);
    c.num("risk.price_band_bps", u64::from(risk.price_band_bps));
    c.num("risk.max_open_orders", u64::from(risk.max_open_orders));
    c.num("risk.orders_per_10s", u64::from(risk.orders_per_10s));
    c.num("risk.orders_per_minute", u64::from(risk.orders_per_minute));
    c.num("risk.margin_ratio_bps", u64::from(risk.margin_ratio_bps));
    c.flag("risk.check_margin", risk.check_margin);
    c.num("risk.countdown_cancel_all_ms", u64::from(risk.countdown_cancel_all_ms));
    c.str("risk.on_strategy_error", name_of(ON_ERROR_NAMES, risk.on_strategy_error));
    c.num("python.callback_budget_us", config.python.callback_budget_us);
    c.num("python.overrun_limit", config.python.overrun_limit);
    c.out
}

/// The operational part: where inputs come from and where outputs go.
#[must_use]
pub fn canonical_operational_text(config: &NodeConfig) -> String {
    let mut c = Canon::default();
    c.str("node.env", env_name(config.node.env));
    c.str("data.catalog", &config.data.catalog);
    match config.data.range {
        Some(r) => {
            c.time("data.range.start", r.start);
            c.time("data.range.end", r.end);
        }
        None => c.raw("data.range", "none"),
    }
    for (i, s) in config.data.streams.iter().enumerate() {
        let p = index_path("data.streams", i);
        c.str(&format!("{p}.venue"), &s.venue);
        c.ids(&format!("{p}.instruments"), &s.instruments);
        c.strs(&format!("{p}.streams"), &s.streams);
        c.str(&format!("{p}.codec"), name_of(CODEC_NAMES, s.codec));
    }
    for (i, v) in config.venues.iter().enumerate() {
        let p = index_path("venues", i);
        c.str(&format!("{p}.endpoint"), name_of(ENDPOINT_NAMES, v.endpoint));
        c.str(&format!("{p}.credentials"), &v.credentials);
        c.str(&format!("{p}.exchange_info"), &v.exchange_info);
    }
    c.num("python.idle_hook_ms", config.python.idle_hook_ms);
    let p = &config.persistence;
    c.str("persistence.mode", name_of(PERSISTENCE_NAMES, p.mode));
    c.str("persistence.dir", &p.dir);
    c.num("persistence.snapshot_every", p.snapshot_every);
    c.num("persistence.sync_every_ms", p.sync_every_ms);
    c.raw(
        "persistence.raw_frames",
        match p.raw_frames {
            RawFrames::Off => "false",
            RawFrames::On => "true",
            RawFrames::Sampled => "\"sampled\"",
        },
    );
    c.flag("persistence.resume", p.resume);
    c.flag("persistence.truncate", p.truncate);
    c.str("telemetry.prometheus", &config.telemetry.prometheus);
    c.flag("telemetry.jsonl", config.telemetry.jsonl);
    c.str("admin.socket", &config.admin.socket);
    c.flag("threads.busy_poll", config.threads.busy_poll);
    c.cpu("threads.core_cpu", config.threads.core_cpu);
    c.cpu("threads.market_cpu", config.threads.market_cpu);
    c.cpu("threads.venue_cpu", config.threads.venue_cpu);
    c.cpu("threads.numa_node", config.threads.numa_node);
    c.num("network.market_idle_timeout_ms", u64::from(config.network.market_idle_timeout_ms));
    c.num("network.venue_idle_timeout_ms", u64::from(config.network.venue_idle_timeout_ms));
    c.out
}

/// SHA-256 over a domain string and the hashed canonical text: what the log header records.
#[must_use]
pub fn config_hash(config: &NodeConfig) -> Digest {
    let mut sha = Sha256::new();
    sha.update(b"jarvis-node-config-v1\n");
    sha.update(canonical_hashed_text(config).as_bytes());
    sha.finish()
}

#[must_use]
pub fn config_hash_hex(config: &NodeConfig) -> String {
    String::from_utf8_lossy(&hex(&config_hash(config))).into_owned()
}

/// What `jarvis-rs config` prints: both parts and the hash.
#[must_use]
pub fn config_report(config: &NodeConfig) -> String {
    let mut out = String::from("# hashed: settings that change what the kernel computes\n");
    out.push_str(&canonical_hashed_text(config));
    out.push_str("# not hashed: where inputs come from and where outputs go\n");
    out.push_str(&canonical_operational_text(config));
    let _ = writeln!(out, "config_hash = {}", config_hash_hex(config));
    out
}

/// The run's `Status` for a config that failed to load.
pub fn status_of_errors(errors: &[ConfigError]) -> Result<()> {
    if errors.is_empty() {
        Ok(())
    } else {
        Err(Status::InvalidArgument)
    }
}
