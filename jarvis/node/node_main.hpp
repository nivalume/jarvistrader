#pragma once

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/node_cli.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/strategy.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// The single-file entry of a pure C++ node (docs/architecture.md section 4.5):
//
//   struct MyStrategy { ... };
//   int main(int argc, char** argv) { return jarvis::node_main<MyStrategy>(argc, argv); }
//
// Strategies are dispatched statically (StaticStrategySet); the node does not link Python.
// The i-th strategy type is built from the i-th [[strategies]] entry (see construct_strategy).

namespace jarvis {

namespace node::detail {

template <strategy::Strategy... S, std::size_t... I>
bool build_strategies(const NodeConfig& config, std::tuple<std::optional<S>...>& out,
                      std::string& error, std::index_sequence<I...> /*indices*/) {
  bool good = true;
  const auto build_one = [&](auto& slot, std::size_t index) {
    if (!good) {
      return;
    }
    StrategyConfig entry;
    if (!strategy_entry(config, index, sizeof...(S), entry, error)) {
      good = false;
      return;
    }
    const core::Status s = construct_strategy(StrategyParams{entry}, slot);
    if (!core::ok(s)) {
      error = "strategy " + entry.id + ": " + std::string{core::to_string(s)};
      good = false;
    }
  };
  (build_one(std::get<I>(out), I), ...);
  return good;
}

template <strategy::Strategy... S, std::size_t... I>
strategy::StaticStrategySet<S...> make_set(std::tuple<std::optional<S>...>& built,
                                           std::index_sequence<I...> /*indices*/) {
  // build_strategies filled every slot or the node stopped before getting here.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return strategy::StaticStrategySet<S...>{std::move(*std::get<I>(built))...};
}

} // namespace node::detail

namespace node::detail {

// What node_main does with a built configuration and strategy set when not replaying.
struct BacktestRunner {
  template <strategy::StrategySet SS>
  int operator()(const std::string& program, const NodeArgs& parsed, const NodeConfig& config,
                 RunManifest manifest, SS& set) const {
    BacktestRequest request;
    request.config = &config;
    request.manifest = std::move(manifest);
    request.out = parsed.out;
    BacktestResult result;
    std::string error;
    if (!core::ok(run_backtest(request, set, result, error))) {
      std::cerr << program << ": " << error << "\n";
      return kExitFailed;
    }
    return print_backtest(std::cout, result);
  }
};

// The shared body of node_main and the live shell's entry: arguments, configuration, strategies,
// replay; a run goes to `runner`.
template <typename Runner, strategy::Strategy... S>
int node_main_with(int argc, char** argv, const Runner& runner) {
  static_assert(sizeof...(S) > 0, "node_main needs at least one strategy type");
  const std::string program = argc > 0 ? std::string{argv[0]} : std::string{"node"};
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  NodeArgs parsed;
  std::string error;
  if (!parse_node_args(args, parsed, error)) {
    std::cerr << program << ": " << error << "\n" << node_usage(program);
    return kExitUsage;
  }
  if (parsed.help) {
    std::cout << node_usage(program);
    return kExitOk;
  }
  NodeConfig config;
  RunManifest manifest;
  const core::Status loaded = parsed.replay.empty()
                                  ? load_node_config(parsed, config, manifest, error)
                                  : load_run_config(parsed.replay, config, error);
  if (!core::ok(loaded)) {
    std::cerr << program << ": " << error << "\n";
    return kExitFailed;
  }
  std::tuple<std::optional<S>...> built;
  if (!build_strategies<S...>(config, built, error, std::index_sequence_for<S...>{})) {
    std::cerr << program << ": " << error << "\n";
    return kExitFailed;
  }
  auto set = make_set<S...>(built, std::index_sequence_for<S...>{});
  if (!parsed.replay.empty()) {
    ReplayOptions options;
    options.until = parsed.until;
    options.dump_state = parsed.dump_state;
    ReplayReport report;
    if (!core::ok(replay_run(parsed.replay, config, set, options, report, error))) {
      std::cerr << program << ": " << error << "\n";
      return kExitFailed;
    }
    return print_replay(std::cout, parsed.replay, report);
  }
  return runner(program, parsed, config, std::move(manifest), set);
}

} // namespace node::detail

template <strategy::Strategy... S> int node_main(int argc, char** argv) {
  return node::detail::node_main_with<node::detail::BacktestRunner, S...>(argc, argv, {});
}

} // namespace jarvis
