#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "jarvis/core/sha256.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"

// Typed node configuration (docs/architecture.md section 4.2). The TOML text is parsed in the
// shell; unknown keys, wrong types and invalid values are errors, every one of them reported with
// its path. Floating-point values are rejected everywhere: decimals are written as strings so
// they parse exactly into fixed point.

namespace jarvis::node {

enum class Env : std::uint8_t { Backtest, Sandbox, Live };
enum class AccountMode : std::uint8_t { OneWay, Hedge };
enum class OmsKind : std::uint8_t { Netting, Hedging };
enum class Endpoint : std::uint8_t { Prod, Testnet };
enum class FillModel : std::uint8_t { TopOfBook, QueuePosition };
enum class SelfTradePrevention : std::uint8_t { None, ExpireTaker, ExpireMaker, ExpireBoth };
enum class Codec : std::uint8_t { Json, Sbe };
enum class OnStrategyError : std::uint8_t { HaltStrategy, HaltNode, Ignore };
enum class PersistenceMode : std::uint8_t { None, Async, Barrier };
enum class RawFrames : std::uint8_t { Off, On, Sampled };

struct Capacity {
  std::uint32_t orders = 4096;
  std::uint32_t instruments = 64;
  std::uint32_t batch = 1024;
  std::uint32_t timers = 256;
  std::uint32_t strategies = 8;
};

struct NodeSection {
  std::string id; // also the ClientOrderId node tag: 1-8 ASCII letters or digits
  Env env = Env::Backtest;
  std::uint64_t seed = 0;
  bool strict_determinism = true;
  Capacity capacity;
  // How a sandbox or live node stops (section 19.4); the Shutdown input records it.
  model::ShutdownMode shutdown = model::ShutdownMode::CancelAllThenExit;
  std::uint32_t shutdown_timeout_ms = 10'000; // the wait for the cancels to be confirmed
  // Market data that is up but silent this long counts as stale: the node goes Degraded until it
  // flows again (section 19.3). 0: no check. It changes the kernel's steps, so it is hashed.
  std::uint32_t market_data_stale_ms = 10'000;
};

struct TimeRange {
  core::UnixNanos start;
  core::UnixNanos end;
};

struct DataStream {
  std::string venue;
  std::vector<model::InstrumentId> instruments;
  std::vector<std::string> streams;
  Codec codec = Codec::Json;
};

struct DataSection {
  std::string catalog;
  std::optional<TimeRange> range;
  std::vector<DataStream> streams;
};

struct Latency {
  std::uint64_t feed_ns = 0;
  std::uint64_t out_ns = 0;
  std::uint64_t in_ns = 0;
  std::uint64_t jitter_ns = 0;
};

struct SimSection {
  FillModel fill_model = FillModel::TopOfBook;
  Latency latency;
  std::string fee_schedule;           // empty: binance_usdm_vip0
  std::vector<model::Money> balances; // the simulated account's starting balances
  SelfTradePrevention stp = SelfTradePrevention::None;
};

struct VenueConfig {
  std::string id;
  std::string kind;
  Endpoint endpoint = Endpoint::Prod;
  std::string credentials; // a reference such as "env:NAME", never the secret itself
  // Sandbox and live: instruments from this saved exchangeInfo response instead of the
  // endpoint's REST API (empty: fetch).
  std::string exchange_info;
  AccountMode account_mode = AccountMode::OneWay;
  OmsKind oms = OmsKind::Netting;
  std::uint32_t leverage =
      0; // initial margin = notional / leverage; 0: the instrument's margin_init
  std::optional<SimSection> sim;
};

// Strategy parameters are defined by the strategy, so the node only checks their types: booleans,
// integers, strings and arrays of those. Nested tables are flattened to dotted keys.
using ParamScalar = std::variant<bool, std::int64_t, std::string>;
using ParamValue = std::variant<bool, std::int64_t, std::string, std::vector<ParamScalar>>;

struct Param {
  std::string key;
  ParamValue value;
};

struct StrategyConfig {
  std::string id;
  std::string impl; // "py:<class>" or "cpp:<registered name>"
  std::vector<model::InstrumentId> instruments;
  std::vector<Param> params; // sorted by key
};

struct RiskSection {
  model::TradingState initial_state = model::TradingState::Active;
  std::optional<model::Money> max_order_notional;
  std::optional<model::Money> max_position_notional; // per instrument, open orders included
  std::optional<model::Money> daily_loss_limit;      // Reducing
  std::optional<model::Money> daily_loss_halt;       // Halted and KillSwitch
  std::optional<model::Money> max_drawdown;          // Reducing
  std::uint32_t price_band_bps = 0;                  // 0: off
  std::uint32_t max_open_orders = 0;                 // per instrument; 0: off
  std::uint32_t orders_per_10s = 250;                // 0: off
  std::uint32_t orders_per_minute = 1000;            // 0: off
  std::uint32_t margin_ratio_bps = 8000;             // Reducing at this maintenance/equity; 0: off
  bool check_margin = true;
  std::uint32_t countdown_cancel_all_ms = 120'000; // env = "live"; 0: off, else >= 10000
  OnStrategyError on_strategy_error = OnStrategyError::HaltStrategy;
};

struct PythonSection {
  std::uint64_t callback_budget_us = 2'000;
  std::uint64_t overrun_limit = 50;
  // Sandbox and live: while idle, the node runs gc.collect(0) and the strategies' on_idle()
  // at most this often, holding the GIL (0: never). Operational: outside the config hash.
  std::uint64_t idle_hook_ms = 100;
};

struct PersistenceSection {
  PersistenceMode mode = PersistenceMode::Async;
  std::string dir = "runs/{node_id}/{run_id}";
  std::uint64_t snapshot_every = 1'000'000;
  std::uint64_t sync_every_ms =
      100; // async: the persist thread's fdatasync cadence; 0: every batch
  RawFrames raw_frames = RawFrames::On;
  // Live: continue the latest earlier run of this node from its snapshots and log (recovery.hpp).
  bool resume = false;
  // Sandbox and live: after each complete snapshot, remove the log segments and snapshots that
  // only precede it.
  bool truncate = false;
};

struct TelemetrySection {
  std::string prometheus;
  bool jsonl = true;
};

struct AdminSection {
  std::string socket;
};

struct NodeConfig {
  NodeSection node;
  DataSection data;
  std::vector<VenueConfig> venues;
  std::vector<StrategyConfig> strategies;
  RiskSection risk;
  PythonSection python;
  PersistenceSection persistence;
  TelemetrySection telemetry;
  AdminSection admin;
};

struct ConfigError {
  std::string path;   // e.g. "venues[0].oms"; empty for errors about the whole document
  std::uint32_t line; // 1-based line in the source text, 0 when the value came from --set
  std::string message;
};

// Command-line overrides. `env` is applied first, then each "a.b.c=value" in order. A path
// segment selects a table key, an array element by index, or the element of an array of tables
// whose `id` equals the segment (e.g. "strategies.mm-001.params.size=0.020"). The value is read
// as a TOML value when it parses as one (numbers, booleans, quoted strings, arrays); otherwise,
// and for anything that looks like a floating-point number, it is taken as a plain string.
struct ConfigOverrides {
  std::optional<std::string> env;
  std::vector<std::string> sets;
};

// Parses `text` (named `source` in messages) and applies `overrides`. Returns InvalidArgument and
// fills `errors` when anything is wrong; `out` is only written on success.
[[nodiscard]] core::Status parse_config(std::string_view text, std::string_view source,
                                        const ConfigOverrides& overrides, NodeConfig& out,
                                        std::vector<ConfigError>& errors);
[[nodiscard]] core::Status load_config(const std::string& path, const ConfigOverrides& overrides,
                                       NodeConfig& out, std::vector<ConfigError>& errors);

// Canonical form: one "path = value" line per field, defaults included, fixed order, strategy
// parameters sorted. The hashed part covers everything that can change what the kernel computes.
// The operational part (node.env, [data], venue endpoints and credential references,
// [persistence], [telemetry], [admin]) is left out of the hash: it says where inputs come from
// and where outputs go, and the inputs themselves are recorded in the event log. This is what
// lets a sandbox recording replay in backtest under the same hash (section 4.6).
[[nodiscard]] std::string canonical_hashed_text(const NodeConfig& config);
[[nodiscard]] std::string canonical_operational_text(const NodeConfig& config);
[[nodiscard]] core::Sha256::Digest config_hash(const NodeConfig& config);

// "source:line: path: message" per error, one per line.
[[nodiscard]] std::string format_errors(std::string_view source,
                                        const std::vector<ConfigError>& errors);

[[nodiscard]] std::string_view to_string(Env value) noexcept;

} // namespace jarvis::node
