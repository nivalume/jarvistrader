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

template <strategy::Strategy... S> int node_main(int argc, char** argv) {
  static_assert(sizeof...(S) > 0, "node_main needs at least one strategy type");
  const std::string program = argc > 0 ? std::string{argv[0]} : std::string{"node"};
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  node::NodeArgs parsed;
  std::string error;
  if (!node::parse_node_args(args, parsed, error)) {
    std::cerr << program << ": " << error << "\n" << node::node_usage(program);
    return node::kExitUsage;
  }
  if (parsed.help) {
    std::cout << node::node_usage(program);
    return node::kExitOk;
  }
  node::NodeConfig config;
  node::RunManifest manifest;
  const core::Status loaded = parsed.replay.empty()
                                  ? node::load_node_config(parsed, config, manifest, error)
                                  : node::load_run_config(parsed.replay, config, error);
  if (!core::ok(loaded)) {
    std::cerr << program << ": " << error << "\n";
    return node::kExitFailed;
  }
  std::tuple<std::optional<S>...> built;
  if (!node::detail::build_strategies<S...>(config, built, error,
                                            std::index_sequence_for<S...>{})) {
    std::cerr << program << ": " << error << "\n";
    return node::kExitFailed;
  }
  auto set = node::detail::make_set<S...>(built, std::index_sequence_for<S...>{});
  if (!parsed.replay.empty()) {
    node::ReplayOptions options;
    options.until = parsed.until;
    options.dump_state = parsed.dump_state;
    node::ReplayReport report;
    if (!core::ok(node::replay_run(parsed.replay, config, set, options, report, error))) {
      std::cerr << program << ": " << error << "\n";
      return node::kExitFailed;
    }
    return node::print_replay(std::cout, parsed.replay, report);
  }
  node::BacktestRequest request;
  request.config = &config;
  request.manifest = std::move(manifest);
  request.out = parsed.out;
  node::BacktestResult result;
  if (!core::ok(node::run_backtest(request, set, result, error))) {
    std::cerr << program << ": " << error << "\n";
    return node::kExitFailed;
  }
  return node::print_backtest(std::cout, result);
}

} // namespace jarvis
