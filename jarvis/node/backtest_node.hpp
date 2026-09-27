#pragma once

#include <memory>
#include <string>
#include <vector>

#include "jarvis/backtest/driver.hpp"
#include "jarvis/backtest/merge_source.hpp"
#include "jarvis/backtest/venue_loop.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/log_source.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// The backtest node (docs/architecture.md section 4): configuration -> catalog sources ->
// merge -> driver -> engine, with every input and output written to the run log.

namespace jarvis::node {

// Called with every input before it is recorded and stepped. The Python node uses it to give
// the GIL back between batches (docs/architecture.md section 7.4).
template <typename H>
concept InputHook = requires(H& hook, const model::Event& event) { hook.before_input(event); };

struct NoHook {
  static void before_input(const model::Event& /*event*/) noexcept {}
};

// The driver's Recorder over the run log.
template <InputHook Hook> class LogRecorder {
public:
  LogRecorder(EventLogWriter& writer, Hook& hook) noexcept : writer_{&writer}, hook_{&hook} {}
  [[nodiscard]] core::Status record(const core::EventKey& key, const model::Event& event) {
    hook_->before_input(event);
    return writer_->append(key, event);
  }
  [[nodiscard]] core::Status emit(const core::EventKey& key, const model::Output& output) {
    return writer_->append_output(key, output);
  }

private:
  EventLogWriter* writer_;
  Hook* hook_;
};

// persistence.mode = "none": nothing is written.
template <InputHook Hook> class NullRecorder {
public:
  explicit NullRecorder(Hook& hook) noexcept : hook_{&hook} {}
  [[nodiscard]] core::Status record(const core::EventKey& /*key*/, const model::Event& event) {
    hook_->before_input(event);
    return core::Status::Ok;
  }
  [[nodiscard]] static core::Status emit(const core::EventKey& /*key*/,
                                         const model::Output& /*output*/) noexcept {
    return core::Status::Ok;
  }

private:
  Hook* hook_;
};

// Kernel capacities and the strategy error policy from [node] and [risk].
[[nodiscard]] strategy::KernelConfig kernel_config(const NodeConfig& config);
[[nodiscard]] strategy::ErrorPolicy error_policy(const NodeConfig& config);
// Strategy i issues orders under [[strategies]][i].id (a nautilus StrategyId, "<name>-<tag>");
// the default "strategy-00<i+1>" stays for entries without a valid one. A strategy whose entry
// lists instruments may trade only those.
void name_strategies(const NodeConfig& config, strategy::KernelServices& kernel);

// What the node steps while Syncing, before strategies start: the instrument definitions found in
// the catalog ({catalog}/{instrument}/instrument/{day}/, the latest day that starts no later than
// data.range, else the earliest) and, with a simulated venue, its starting account. The events
// borrow `balances`, so a Preamble must stay where it is built.
struct Preamble {
  std::vector<model::Event> events;
  std::vector<model::AccountBalance> balances;
};
[[nodiscard]] core::Status load_preamble(const NodeConfig& config, Preamble& out,
                                         std::string& error);
// The simulated venue's starting account ([venues.sim] balances) as an AccountState at `ts`,
// appended to `out`.
[[nodiscard]] core::Status account_preamble(const NodeConfig& config, const SimSection& sim,
                                            core::UnixNanos ts, Preamble& out, std::string& error);

// The simulated venue of [venues.sim]: fills, fees, latency, self-trade prevention.
[[nodiscard]] core::Status venue_loop_config(const NodeConfig& config,
                                             const strategy::KernelConfig& kernel,
                                             backtest::VenueLoopConfig& out, std::string& error);

// Opens one LogSource per catalog stream that [data] selects. The merge keeps pointers into
// `sources`, so it must not be resized afterwards.
[[nodiscard]] core::Status open_sources(const NodeConfig& config, std::vector<LogSource>& sources,
                                        std::string& error);

struct BacktestRequest {
  const NodeConfig* config = nullptr;
  RunManifest manifest; // saved in the run directory
  std::string out;      // run directory; empty for persistence.dir
  HeaderExtras extras;
};

struct BacktestResult {
  std::string directory; // empty when persistence.mode = "none"
  backtest::RunSummary summary;
};

namespace detail {

// Runs the driver over `source`, recording to the run directory unless persistence is off.
template <strategy::StrategySet SS, typename Source, InputHook Hook>
[[nodiscard]] core::Status drive(const BacktestRequest& request, engine::Engine<SS>& engine,
                                 Source& source, const backtest::DriverOptions& options,
                                 BacktestResult& result, std::string& error, Hook& hook) {
  const NodeConfig& config = *request.config;
  core::Status s = core::Status::Ok;
  if (config.persistence.mode == PersistenceMode::None) {
    NullRecorder<Hook> recorder{hook};
    backtest::Driver driver{engine, source, recorder, options};
    s = driver.run(result.summary);
    if (!core::ok(s)) {
      error = "the run stopped: " + std::string{core::to_string(s)};
    }
    return s;
  }
  s = create_run_directory(config, request.manifest, request.out, result.directory, error);
  if (!core::ok(s)) {
    return s;
  }
  EventLogWriter writer;
  EventLogOptions log_options;
  log_options.sync_on_flush = config.persistence.mode == PersistenceMode::Barrier;
  s = writer.open(result.directory, run_header(config, request.extras), log_options);
  if (!core::ok(s)) {
    error = "cannot open the run log in " + result.directory;
    return s;
  }
  LogRecorder<Hook> recorder{writer, hook};
  backtest::Driver driver{engine, source, recorder, options};
  s = driver.run(result.summary);
  const core::Status closed = writer.close();
  if (!core::ok(s)) {
    error = "the run stopped at seq " + std::to_string(engine.kernel().current.seq) + ": " +
            std::string{core::to_string(s)};
    return s;
  }
  if (!core::ok(closed)) {
    error = "cannot finish the run log in " + result.directory;
  }
  return closed;
}

} // namespace detail

template <strategy::StrategySet SS, InputHook Hook>
[[nodiscard]] core::Status run_backtest(const BacktestRequest& request, SS& strategies,
                                        BacktestResult& result, std::string& error, Hook& hook) {
  const NodeConfig& config = *request.config;
  if (config.node.env != Env::Backtest) {
    error = "node.env = \"" + std::string{to_string(config.node.env)} +
            "\" is not available yet; this build runs backtests (sandbox arrives with M4)";
    return core::Status::InvalidArgument;
  }
  if (strategies.size() > config.node.capacity.strategies) {
    error = "the node has " + std::to_string(strategies.size()) +
            " strategies but node.capacity.strategies is " +
            std::to_string(config.node.capacity.strategies);
    return core::Status::CapacityExceeded;
  }
  std::vector<LogSource> sources;
  core::Status s = open_sources(config, sources, error);
  if (!core::ok(s)) {
    return s;
  }
  backtest::MergeSource<LogSource> merge{sources.size()};
  for (LogSource& source : sources) {
    s = merge.add(source);
    if (!core::ok(s)) {
      return s;
    }
  }
  backtest::DriverOptions options;
  if (config.data.range) {
    options.start = config.data.range->start;
    options.end = config.data.range->end;
  }
  Preamble preamble;
  s = load_preamble(config, preamble, error);
  if (!core::ok(s)) {
    return s;
  }
  options.preamble = preamble.events;
  engine::Engine<SS> engine{kernel_config(config), strategies, error_policy(config)};
  name_strategies(config, engine.kernel());
  if (config.venues.empty() || !config.venues.front().sim) {
    return detail::drive(request, engine, merge, options, result, error, hook);
  }
  backtest::VenueLoopConfig venue;
  s = venue_loop_config(config, engine.kernel().config(), venue, error);
  if (!core::ok(s)) {
    return s;
  }
  // Large (the in-flight queues): on the heap.
  auto loop = std::make_unique<backtest::VenueLoop<backtest::MergeSource<LogSource>>>(venue, merge);
  const core::FixedVector<model::StrategyId>& ids = engine.kernel().trading.strategy_ids;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    loop->exchange().set_strategy_id(static_cast<std::uint16_t>(i), ids[i]);
  }
  for (const model::Event& e : preamble.events) {
    s = loop->exchange().on_data(e, options.start.value_or(core::UnixNanos{}));
    if (!core::ok(s)) {
      error = "the simulated venue refused the preamble: " + std::string{core::to_string(s)};
      return s;
    }
  }
  return detail::drive(request, engine, *loop, options, result, error, hook);
}

template <strategy::StrategySet SS>
[[nodiscard]] core::Status run_backtest(const BacktestRequest& request, SS& strategies,
                                        BacktestResult& result, std::string& error) {
  NoHook hook;
  return run_backtest(request, strategies, result, error, hook);
}

} // namespace jarvis::node
