#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/node/run_dir.hpp"

// The command line every node entry shares: node_main<S...> in C++ and jarvis.main() in Python
// (docs/architecture.md section 4.5).
//
//   PROGRAM --config FILE [--env ENV] [--set path=value]... [--out DIR]
//   PROGRAM --replay RUN_DIR [--until SEQ] [--dump-state]

namespace jarvis::node {

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitDivergence = 3;

struct NodeArgs {
  std::string config;
  ConfigOverrides overrides;
  std::string out;
  std::string replay;
  std::optional<std::uint64_t> until;
  bool dump_state = false;
  bool help = false;
};

// Parses the arguments after the program name. False with `error` set on a bad command line.
[[nodiscard]] bool parse_node_args(const std::vector<std::string>& args, NodeArgs& out,
                                   std::string& error);
[[nodiscard]] std::string node_usage(std::string_view program);

// Loads --config with the overrides and fills the manifest saved in the run directory.
[[nodiscard]] core::Status load_node_config(const NodeArgs& args, NodeConfig& config,
                                            RunManifest& manifest, std::string& error);

// The StrategyConfig for the i-th strategy of a node built from `count` strategy types: the
// i-th [[strategies]] entry, or an empty entry "strategy-00<i+1>" when the file lists none.
// False with `error` set when the file lists a different number of strategies.
[[nodiscard]] bool strategy_entry(const NodeConfig& config, std::size_t index, std::size_t count,
                                  StrategyConfig& out, std::string& error);

// Human-readable reports; they return the process exit code.
int print_backtest(std::ostream& out, const BacktestResult& result);
int print_replay(std::ostream& out, const std::string& directory, const ReplayReport& report);

} // namespace jarvis::node
