// The backtest node end to end: catalog logs -> run -> run directory -> replay.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/fingerprint.hpp"
#include "jarvis/node/log_source.hpp"
#include "jarvis/node/node_cli.hpp"
#include "jarvis/node/node_main.hpp"
#include "jarvis/node/recovery.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/node/snapshot_file.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"

namespace {

namespace node = jarvis::node;
namespace st = jarvis::strategy;
namespace m = jarvis::model;
namespace wire = jarvis::model::wire;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

class TempDir {
public:
  explicit TempDir(std::string_view name) {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("jarvis-run-" + std::string{name} + "-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  [[nodiscard]] std::string sub(std::string_view name) const { return (path_ / name).string(); }

private:
  std::filesystem::path path_;
};

m::InstrumentId btc() {
  m::InstrumentId id;
  REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
}

constexpr std::uint64_t kDay1 = 1'788'220'800'000'000'000ULL; // 2026-09-01T00:00:00Z
constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::uint64_t kHour = 3'600 * kSecond;
constexpr std::uint64_t kDay = 24 * kHour;

m::Event trade(std::uint64_t ts, std::int64_t price_tenths) {
  m::TradeTick t;
  t.instrument_id = btc();
  REQUIRE(m::Price::from_raw(price_tenths * 100'000'000, 1, t.price) == Status::Ok);
  REQUIRE(m::Quantity::from_raw(10'000'000, 3, t.size) == Status::Ok);
  t.aggressor_side = m::AggressorSide::Sell;
  REQUIRE(m::TradeId::from(std::to_string(ts % 1'000'000'007ULL), t.trade_id) == Status::Ok);
  t.ts_event = UnixNanos{ts};
  t.ts_init = UnixNanos{ts};
  return m::Event{t};
}

m::Event quote(std::uint64_t ts, std::int64_t bid_tenths) {
  m::QuoteTick q;
  q.instrument_id = btc();
  REQUIRE(m::Price::from_raw(bid_tenths * 100'000'000, 1, q.bid_price) == Status::Ok);
  REQUIRE(m::Price::from_raw((bid_tenths + 1) * 100'000'000, 1, q.ask_price) == Status::Ok);
  REQUIRE(m::Quantity::from_raw(1'000'000'000, 3, q.bid_size) == Status::Ok);
  REQUIRE(m::Quantity::from_raw(2'000'000'000, 3, q.ask_size) == Status::Ok);
  q.ts_event = UnixNanos{ts};
  q.ts_init = UnixNanos{ts};
  return m::Event{q};
}

// Writes one catalog day: `count` events every `step` from `start`.
void write_day(const std::string& catalog, std::string_view stream, std::string_view day,
               std::uint16_t source, std::uint64_t start, std::uint64_t step, int count,
               bool trades) {
  const std::string dir =
      node::catalog_directory(catalog, btc(), std::string{stream}, std::string{day});
  node::EventLogWriter writer;
  REQUIRE(writer.open(dir, wire::LogHeader{}, {}) == Status::Ok);
  for (int i = 0; i < count; ++i) {
    const std::uint64_t ts = start + static_cast<std::uint64_t>(i) * step;
    const auto px = static_cast<std::int64_t>(650'000 + (i * 7) % 13);
    const EventKey key{UnixNanos{ts}, source, static_cast<std::uint64_t>(i + 1)};
    REQUIRE(writer.append(key, trades ? trade(ts, px) : quote(ts, px)) == Status::Ok);
  }
  REQUIRE(writer.close() == Status::Ok);
}

// Records the last trade price and a heartbeat every `timer_ms`, and optionally shifts the
// recorded price (to provoke a divergence against a recording made without the shift).
// NOLINTBEGIN(readability-make-member-function-const)
struct Echo {
  std::int64_t timer_ms = 0;
  std::int64_t shift_raw = 0;
  std::uint64_t heartbeats = 0;

  template <typename Ar> void state(Ar& ar) { ar(heartbeats); } // its snapshots are complete

  static Status create(const node::StrategyParams& params, Echo& out) {
    Status s = params.get_or<std::int64_t>("timer_ms", 0, out.timer_ms);
    if (jarvis::core::ok(s)) {
      s = params.get_or<std::int64_t>("shift_raw", 0, out.shift_raw);
    }
    return s;
  }
  Status on_start(st::Context& ctx) {
    Status s = ctx.subscribe_trades(btc());
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_quotes(btc(), jarvis::data::Cadence::conflated());
    }
    if (jarvis::core::ok(s) && timer_ms > 0) {
      const auto period = static_cast<std::uint64_t>(timer_ms) * 1'000'000ULL;
      s = ctx.set_timer(1, UnixNanos{ctx.now().value() + period},
                        jarvis::core::DurationNanos{period});
    }
    return s;
  }
  Status on_trade(st::Context& ctx, const m::TradeTick& t) {
    m::Decimal px;
    const Status s = m::Decimal::from_raw(t.price.raw() + shift_raw, t.price.precision(), px);
    return jarvis::core::ok(s) ? ctx.record("last", px) : s;
  }
  Status on_timer(st::Context& ctx, jarvis::core::TimerKey /*key*/, UnixNanos /*deadline*/) {
    ++heartbeats;
    m::Decimal n;
    static_cast<void>(
        m::Decimal::from_raw(static_cast<std::int64_t>(heartbeats) * 1'000'000'000, 0, n));
    return ctx.record("beat", n);
  }
};
// NOLINTEND(readability-make-member-function-const)

std::string config_text(const std::string& catalog, std::string_view params) {
  return "[node]\nid = \"bt01\"\nseed = 7\n\n[data]\ncatalog = \"" + catalog +
         "\"\nrange = { start = \"2026-09-01T00:00:00Z\", end = \"2026-09-02T12:00:00Z\" }\n\n"
         "[[data.streams]]\nvenue = \"BINANCE_USDM\"\n"
         "instruments = [\"BTCUSDT-PERP.BINANCE\"]\n"
         "streams = [\"aggTrade\", \"bookTicker\"]\n\n"
         "[[venues]]\nid = \"BINANCE_USDM\"\nkind = \"binance_usdm\"\n\n"
         "[[strategies]]\nid = \"echo-001\"\nimpl = \"cpp:test.Echo\"\nparams = { " +
         std::string{params} + " }\n";
}

// A catalog with two days of trades (the second partly out of range, one day beyond it) and
// one day of quotes whose timestamps collide with the trades'.
std::string make_catalog(const TempDir& tmp) {
  const std::string catalog = tmp.sub("catalog");
  write_day(catalog, "aggTrade", "2026-09-01", 1, kDay1 + kSecond, kSecond / 4, 40, true);
  write_day(catalog, "aggTrade", "2026-09-02", 1, kDay1 + kDay, 6 * kHour, 4, true);
  write_day(catalog, "aggTrade", "2026-09-05", 1, kDay1 + (4 * kDay), kSecond, 3, true);
  write_day(catalog, "bookTicker", "2026-09-01", 2, kDay1 + kSecond, kSecond / 2, 20, false);
  return catalog;
}

Echo make_echo(const node::StrategyConfig& entry) {
  std::optional<Echo> echo;
  REQUIRE(node::construct_strategy(node::StrategyParams{entry}, echo) == Status::Ok);
  return echo.value_or(Echo{});
}

node::NodeConfig parse(const std::string& text) {
  node::NodeConfig config;
  std::vector<node::ConfigError> errors;
  const Status s = node::parse_config(text, "test.toml", {}, config, errors);
  INFO(node::format_errors("test.toml", errors));
  REQUIRE(s == Status::Ok);
  return config;
}

struct Recorded {
  std::string directory;
  node::BacktestResult result;
};

Recorded record_run(const TempDir& tmp, const std::string& text, std::string_view out_name) {
  const node::NodeConfig config = parse(text);
  st::StaticStrategySet<Echo> set{make_echo(config.strategies[0])};
  node::BacktestRequest request;
  request.config = &config;
  request.manifest = node::RunManifest{text, "test.toml", {}, {}};
  request.out = tmp.sub(out_name);
  Recorded r;
  std::string error;
  const Status s = node::run_backtest(request, set, r.result, error);
  INFO(error);
  REQUIRE(s == Status::Ok);
  r.directory = r.result.directory;
  return r;
}

// Replays `directory`; a non-empty `param` adds or replaces that strategy parameter.
node::ReplayReport replay(const std::string& directory, std::string_view param,
                          std::int64_t value = 0, const node::ReplayOptions& options = {}) {
  node::NodeConfig config;
  std::string error;
  const Status loaded = node::load_run_config(directory, config, error);
  INFO(error);
  REQUIRE(loaded == Status::Ok);
  node::StrategyConfig entry = config.strategies[0];
  if (!param.empty()) {
    std::erase_if(entry.params, [&](const node::Param& p) { return p.key == param; });
    entry.params.push_back(node::Param{std::string{param}, value});
    std::sort(entry.params.begin(), entry.params.end(),
              [](const node::Param& a, const node::Param& b) { return a.key < b.key; });
  }
  st::StaticStrategySet<Echo> set{make_echo(entry)};
  node::ReplayReport report;
  REQUIRE(node::replay_run(directory, config, set, options, report, error) == Status::Ok);
  return report;
}

// Echo without a state of its own: a node running it cannot resume.
struct Forgetful {
  static Status on_start(st::Context& ctx) { return ctx.subscribe_trades(btc()); }
};

using EchoEngine = jarvis::engine::Engine<st::StaticStrategySet<Echo>>;

// The kernel state of `engine` (Engine::save_state).
std::vector<std::byte> state_of(EchoEngine& engine) {
  std::vector<std::byte> out;
  REQUIRE(engine.save_state(out) == Status::Ok);
  return out;
}

// Copies the run directory `from` to `to`, rewriting its log into segments of about
// `segment_bytes`; with `truncate_at`, the segments before the snapshot of that seq are then
// removed as persistence.truncate does.
void copy_run(const std::string& from, const std::string& to, std::uint64_t segment_bytes,
              std::optional<std::uint64_t> truncate_at, std::uint32_t& removed) {
  namespace fs = std::filesystem;
  fs::create_directories(to);
  for (const auto& entry : fs::directory_iterator(from)) {
    if (entry.path().extension() != ".jlog") {
      fs::copy_file(entry.path(), fs::path{to} / entry.path().filename());
    }
  }
  node::EventLogReader reader;
  REQUIRE(reader.open(from) == Status::Ok);
  node::EventLogWriter writer;
  node::EventLogOptions options;
  options.segment_bytes = segment_bytes;
  REQUIRE(writer.open(to, reader.header(), options) == Status::Ok);
  wire::RecordView record;
  while (reader.next(record) == Status::Ok) {
    REQUIRE(writer.append_record(record.bytes) == Status::Ok);
  }
  removed = 0;
  if (truncate_at) {
    REQUIRE(writer.flush() == Status::Ok);
    REQUIRE(writer.truncate_before(*truncate_at, removed) == Status::Ok);
  }
  REQUIRE(writer.close() == Status::Ok);
}

} // namespace

namespace {

// Runs node_main<Echo> with `args` after the program name.
int run_main(std::vector<std::string> args) {
  args.insert(args.begin(), "node");
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& a : args) {
    argv.push_back(a.data());
  }
  return jarvis::node_main<Echo>(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the catalog resolves the days in range, per stream") {
    const TempDir tmp{"catalog"};
    const node::NodeConfig config = parse(config_text(make_catalog(tmp), "timer_ms = 3600000"));
    std::vector<node::CatalogStream> streams;
    std::string error;
    REQUIRE(node::resolve_catalog(config.data, streams, error) == Status::Ok);
    REQUIRE(streams.size() == 2);
    CHECK(streams[0].stream == "aggTrade");
    CHECK(streams[0].days.size() == 2); // 2026-09-05 is outside the range
    CHECK(streams[1].stream == "bookTicker");
    CHECK(streams[1].days.size() == 1);

    node::NodeConfig missing = config;
    missing.data.streams[0].streams.emplace_back("markPrice@1s");
    CHECK(node::resolve_catalog(missing.data, streams, error) == Status::NotFound);
    CHECK(error.find("markPrice@1s") != std::string::npos);
  }

  TEST_CASE("a log source chains days and skips output records") {
    const TempDir tmp{"source"};
    const std::string a = tmp.sub("a");
    const std::string b = tmp.sub("b");
    {
      node::EventLogWriter writer;
      REQUIRE(writer.open(a, wire::LogHeader{}, {}) == Status::Ok);
      REQUIRE(writer.append(EventKey{UnixNanos{1}, 1, 1}, trade(1, 10)) == Status::Ok);
      REQUIRE(writer.append_output(EventKey{UnixNanos{1}, 0, 1}, m::Output{m::FeatureUpdate{}}) ==
              Status::Ok);
      REQUIRE(writer.append(EventKey{UnixNanos{2}, 1, 2}, trade(2, 10)) == Status::Ok);
      REQUIRE(writer.close() == Status::Ok);
    }
    {
      node::EventLogWriter writer;
      REQUIRE(writer.open(b, wire::LogHeader{}, {}) == Status::Ok);
      REQUIRE(writer.append(EventKey{UnixNanos{2}, 1, 1}, trade(2, 10)) == Status::Ok);
      REQUIRE(writer.close() == Status::Ok);
    }
    node::LogSource source;
    REQUIRE(source.open({a, b}) == Status::Ok);
    EventKey key;
    m::Event event;
    REQUIRE(source.next(key, event) == Status::Ok);
    CHECK(key == EventKey{UnixNanos{1}, 1, 1});
    REQUIRE(source.next(key, event) == Status::Ok);
    CHECK(key == EventKey{UnixNanos{2}, 1, 2});
    // The second day starts at the same ts; its seq continues after the first day's.
    REQUIRE(source.next(key, event) == Status::Ok);
    CHECK(key == EventKey{UnixNanos{2}, 1, 3});
    CHECK(source.next(key, event) == Status::EndOfStream);
  }

  TEST_CASE("a backtest run replays without divergence") {
    const TempDir tmp{"replay"};
    const std::string text = config_text(make_catalog(tmp), "timer_ms = 3600000");
    const Recorded run = record_run(tmp, text, "run");
    const auto& summary = run.result.summary;
    CHECK(summary.state == m::NodeState::Stopped);
    CHECK(summary.data_events == 40 + 2 + 20); // day 2 stops at 12:00
    CHECK(summary.skipped == 1);               // the first event at or after the end
    CHECK(summary.timers == 35);               // hourly from 00:00:01 until 12:00 the next day
    CHECK(summary.outputs == 42 + summary.timers);
    CHECK(std::filesystem::exists(run.directory + "/config.toml"));
    CHECK(std::filesystem::exists(run.directory + "/run.toml"));

    const node::ReplayReport report = replay(run.directory, "");
    INFO((report.divergence ? report.divergence->recorded + " / " + report.divergence->replayed
                            : std::string{}));
    CHECK_FALSE(report.divergence.has_value());
    CHECK(report.inputs == summary.inputs);
    CHECK(report.outputs == summary.outputs);

    node::ReplayOptions options;
    options.until = 12;
    options.dump_state = true;
    const node::ReplayReport partial = replay(run.directory, "", 0, options);
    CHECK_FALSE(partial.divergence.has_value());
    CHECK(partial.inputs == 12);
    CHECK(partial.state.find("at seq=12") != std::string::npos);
    CHECK(partial.state.find("strategy 0 Trade BTCUSDT-PERP.BINANCE every") != std::string::npos);
    CHECK(partial.state.find("strategy 0 Quote BTCUSDT-PERP.BINANCE conflated") !=
          std::string::npos);
    CHECK(partial.state.find("owner=0 id=1 periodic") != std::string::npos);
  }

  TEST_CASE("a changed strategy is reported at the first differing output") {
    const TempDir tmp{"diverge"};
    const Recorded run =
        record_run(tmp, config_text(make_catalog(tmp), "timer_ms = 3600000"), "run");
    const node::ReplayReport report = replay(run.directory, "shift_raw", 100'000'000); // +0.1
    REQUIRE(report.divergence.has_value());
    const node::Divergence d = report.divergence.value_or(node::Divergence{});
    CHECK(d.recorded.find("StrategyRecord") != std::string::npos);
    CHECK(d.replayed.find("StrategyRecord") != std::string::npos);
    CHECK(d.recorded != d.replayed);
    // The first trade is input 6: the four lifecycle transitions happen at the range start and
    // their batch closes (input 5) before the first trade one second later.
    CHECK(d.seq == 6);
  }

  TEST_CASE("a run directory whose configuration was edited is refused") {
    const TempDir tmp{"edited"};
    const Recorded run =
        record_run(tmp, config_text(make_catalog(tmp), "timer_ms = 3600000"), "run");
    {
      std::ofstream file(run.directory + "/config.toml", std::ios::app);
      file << "\n[python]\ncallback_budget_us = 99\n";
    }
    node::NodeConfig config;
    std::string error;
    CHECK(node::load_run_config(run.directory, config, error) == Status::InvalidState);
    CHECK(error.find("config hash") != std::string::npos);
  }

  TEST_CASE("the same inputs give the same run log") {
    const TempDir tmp{"twice"};
    const std::string text = config_text(make_catalog(tmp), "timer_ms = 600000");
    const Recorded a = record_run(tmp, text, "a");
    const Recorded b = record_run(tmp, text, "b");
    node::LogComparison c;
    REQUIRE(node::compare_logs(a.directory, b.directory, node::RecordFilter::All, c) == Status::Ok);
    CHECK(c.equal);
    CHECK(c.compared == a.result.summary.inputs + a.result.summary.outputs);
  }

  TEST_CASE("node_main runs a configuration and replays the run") {
    const TempDir tmp{"main"};
    const std::string config_path = tmp.sub("node.toml");
    {
      std::ofstream file(config_path);
      file << config_text(make_catalog(tmp), "timer_ms = 1800000");
    }
    const std::string out = tmp.sub("run");
    CHECK(run_main({"--config", config_path, "--out", out, "--set", "node.seed=9"}) ==
          node::kExitOk);
    CHECK(run_main({"--replay", out}) == node::kExitOk);
    CHECK(run_main({"--replay", out, "--set", "node.seed=1"}) == node::kExitUsage);
    // The override is part of the run: it is saved in run.toml and the hash checks out.
    node::NodeConfig config;
    std::string error;
    REQUIRE(node::load_run_config(out, config, error) == Status::Ok);
    CHECK(config.node.seed == 9);
  }

  TEST_CASE("node arguments are checked") {
    node::NodeArgs args;
    std::string error;
    CHECK_FALSE(node::parse_node_args({}, args, error));
    CHECK_FALSE(node::parse_node_args({"--config", "a", "--replay", "b"}, args, error));
    args = {};
    CHECK_FALSE(node::parse_node_args({"--config", "a", "--until", "5"}, args, error));
    args = {};
    CHECK_FALSE(node::parse_node_args({"--replay", "r", "--until", "x"}, args, error));
    args = {};
    CHECK(node::parse_node_args({"--replay", "r", "--until", "5", "--dump-state"}, args, error));
    CHECK(args.until == std::optional<std::uint64_t>{5});
    CHECK(args.dump_state);
  }

  TEST_CASE("a log whose older segments were removed replays from the snapshot covering it") {
    const TempDir tmp{"truncated"};
    const std::string text = config_text(make_catalog(tmp), "timer_ms = 3600000") +
                             "\n[persistence]\nsnapshot_every = 10\n";
    const Recorded run = record_run(tmp, text, "run");
    const std::vector<node::SnapshotEntry> snapshots = node::list_snapshots(run.directory);
    REQUIRE(snapshots.size() >= 4);
    const node::SnapshotEntry& middle = snapshots[snapshots.size() / 2];

    std::uint32_t removed = 0;
    const std::string copy = tmp.sub("copy");
    copy_run(run.directory, copy, 1024, middle.seq, removed);
    CHECK(removed > 0);
    CHECK_FALSE(std::filesystem::exists(copy + "/" + node::segment_name(0)));
    node::EventLogReader reader;
    REQUIRE(reader.open(copy) == Status::Ok);
    CHECK(reader.header().segment_index == removed);
    wire::RecordView first;
    REQUIRE(reader.next(first) == Status::Ok);
    CHECK(first.header.seq > 1);
    CHECK(first.header.seq <= middle.seq);
    // The snapshots older than the first record left are gone; the one truncated at is kept.
    const std::vector<node::SnapshotEntry> kept = node::list_snapshots(copy);
    REQUIRE_FALSE(kept.empty());
    CHECK(kept.front().seq >= first.header.seq);
    CHECK(std::filesystem::exists(middle.path));

    // The replay starts from the earliest snapshot that covers the first record, and checks
    // every snapshot after it.
    const node::ReplayReport report = replay(copy, "");
    INFO((report.divergence ? report.divergence->recorded + " / " + report.divergence->replayed
                            : std::string{}));
    CHECK_FALSE(report.divergence.has_value());
    CHECK(report.start_seq == kept.front().seq);
    CHECK(report.inputs == run.result.summary.inputs - report.start_seq);
    CHECK(report.snapshots_checked == kept.size() - 1);

    // A snapshot older than the log cannot start it.
    node::NodeConfig config;
    std::string error;
    REQUIRE(node::load_run_config(copy, config, error) == Status::Ok);
    st::StaticStrategySet<Echo> set{make_echo(config.strategies[0])};
    node::ReplayOptions options;
    options.from_snapshot = snapshots.front().path;
    node::ReplayReport refused;
    CHECK(node::replay_run(copy, config, set, options, refused, error) == Status::InvalidArgument);
    CHECK(error.find("older than the log") != std::string::npos);
  }

  TEST_CASE("recovery restores the state a run log ends in, wherever a crash cut it") {
    const TempDir tmp{"recover"};
    const std::string text = config_text(make_catalog(tmp), "timer_ms = 3600000") +
                             "\n[persistence]\nsnapshot_every = 10\n";
    const Recorded run = record_run(tmp, text, "run");
    const node::NodeConfig config = parse(text);
    const std::string segment = run.directory + "/" + node::segment_name(0);
    const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(segment));

    std::vector<std::uint64_t> cuts{size, size - 1, size - 3, 10, 0};
    for (std::uint64_t i = 1; i < 40; ++i) {
      cuts.push_back(size * i / 40);
    }
    int n = 0;
    int torn = 0;       // cuts inside a record (or the header)
    int torn_steps = 0; // cuts between an input and its outputs
    for (const std::uint64_t cut : cuts) {
      CAPTURE(cut);
      const std::string crashed = tmp.sub("crashed-" + std::to_string(n++));
      std::filesystem::copy(run.directory, crashed);
      std::filesystem::resize_file(crashed + "/" + node::segment_name(0), cut);
      // A backtest writes its snapshots at once; a live node's persist thread only once the log
      // holds everything before them. So, as a crash leaves it: no snapshot beyond what is left.
      std::uint64_t next_input = 0;
      {
        node::EventLogReader left;
        wire::RecordView record;
        if (left.open(crashed, node::EventLogReadOptions{true}) == Status::Ok) {
          while (left.next(record) == Status::Ok) {
            if (record.header.kind < wire::kFirstOutputKind) {
              next_input = record.header.seq;
            }
          }
        }
      }
      for (const node::SnapshotEntry& e : node::list_snapshots(crashed)) {
        if (e.seq + 1 > next_input) {
          std::filesystem::remove(e.path);
        }
      }

      st::StaticStrategySet<Echo> set{make_echo(config.strategies[0])};
      EchoEngine engine{node::kernel_config(config), set, node::error_policy(config)};
      node::name_strategies(config, engine.kernel());
      node::RecoveryReport report;
      std::string error;
      const Status s = node::recover_run(crashed, config, engine, report, error);
      INFO(error);
      REQUIRE(s == Status::Ok);
      torn += report.torn_bytes != 0 ? 1 : 0;
      torn_steps += report.torn_step ? 1 : 0;

      // The same state as replaying the whole run up to that input.
      st::StaticStrategySet<Echo> ref_set{make_echo(config.strategies[0])};
      EchoEngine reference{node::kernel_config(config), ref_set, node::error_policy(config)};
      node::name_strategies(config, reference.kernel());
      if (report.last_seq != 0) {
        node::ReplayOptions until;
        until.until = report.last_seq;
        node::ReplayReport replayed;
        REQUIRE(node::replay_into(run.directory, config, reference, until, replayed, error) ==
                Status::Ok);
        REQUIRE_FALSE(replayed.divergence.has_value());
      }
      CHECK(state_of(engine) == state_of(reference));
      CHECK(set.get<0>().heartbeats == ref_set.get<0>().heartbeats);
    }
    CHECK(torn > 0);
    CHECK(torn_steps > 0);
  }

  TEST_CASE("recovery refuses another configuration and strategies without a state") {
    const TempDir tmp{"refuse"};
    const std::string catalog = make_catalog(tmp);
    const std::string text = config_text(catalog, "timer_ms = 3600000");
    const Recorded run = record_run(tmp, text, "run");
    std::string error;
    node::RecoveryReport report;

    const node::NodeConfig other = parse(config_text(catalog, "timer_ms = 60000"));
    st::StaticStrategySet<Echo> set{make_echo(other.strategies[0])};
    EchoEngine engine{node::kernel_config(other), set, node::error_policy(other)};
    CHECK(node::recover_run(run.directory, other, engine, report, error) ==
          Status::InvalidArgument);
    CHECK(error.find("different configuration") != std::string::npos);

    const node::NodeConfig config = parse(text);
    st::StaticStrategySet<Forgetful> forgetful{Forgetful{}};
    jarvis::engine::Engine<st::StaticStrategySet<Forgetful>> plain{
        node::kernel_config(config), forgetful, node::error_policy(config)};
    CHECK(node::recover_run(run.directory, config, plain, report, error) == Status::InvalidState);
    CHECK(error.find("does not describe its own state") != std::string::npos);
  }

  TEST_CASE("the runs of a node are found where persistence.dir puts them, oldest first") {
    const TempDir tmp{"runs"};
    node::NodeConfig config;
    config.node.id = "mm01";
    config.persistence.dir = tmp.sub("runs") + "/{node_id}/{run_id}";
    const std::string base = tmp.sub("runs") + "/mm01/";
    for (const char* id : {"20260930T120000Z-10", "20260930T120000Z", "20260930T115959Z",
                           "20260930T120000Z-2", "20261001T000000Z-no-log"}) {
      std::filesystem::create_directories(base + id);
      if (!std::string_view{id}.ends_with("no-log")) {
        std::ofstream{base + id + "/" + node::segment_name(0)} << "x";
      }
    }
    CHECK(node::node_runs(config) ==
          std::vector<std::string>{base + "20260930T115959Z", base + "20260930T120000Z",
                                   base + "20260930T120000Z-2", base + "20260930T120000Z-10"});
    CHECK(node::resume_source(config) == base + "20260930T120000Z-10");
    config.node.id = "other";
    CHECK(node::node_runs(config).empty());
  }
}
