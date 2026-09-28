#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>

#include "jarvis/live/live_node.hpp"
#include "jarvis/live/sandbox_node.hpp"
#include "jarvis/node/node_main.hpp"

// The entry of a C++ node built with the live shell (docs/architecture.md section 4.5): the same
// command line as node_main; node.env = "sandbox" runs against the venue's market data and
// node.env = "live" trades at the venue, until SIGINT or SIGTERM (or --run-for):
//
//   int main(int argc, char** argv) { return jarvis::live_node_main<MyStrategy>(argc, argv); }

namespace jarvis {

namespace live::detail {

inline int print_sandbox(std::ostream& out, const SandboxResult& r) {
  const auto& s = r.summary;
  out << "run: " << (r.directory.empty() ? std::string{"(not recorded)"} : r.directory) << "\n"
      << "inputs " << s.inputs << ", outputs " << s.outputs << ", data events " << s.data_events
      << ", venue answers " << s.venue_answers << ", timers " << s.timers << ", strategy errors "
      << s.strategy_errors << "\n"
      << "feed: messages " << r.feed.messages << ", events " << r.feed.events << ", decode errors "
      << r.feed.decode_errors << ", connects " << r.feed.connects << ", snapshots "
      << r.feed.snapshots << " (" << r.feed.snapshot_failures << " failed), book syncs "
      << r.feed.book_syncs << ", ring waits " << r.feed.ring_waits << "\n";
  if (s.left_open != 0) {
    out << "left open at stop: " << s.left_open << " orders\n";
  }
  return s.halted ? node::kExitFailed : node::kExitOk;
}

inline int print_live(std::ostream& out, const LiveResult& r) {
  const auto& s = r.summary;
  out << "run: " << (r.directory.empty() ? std::string{"(not recorded)"} : r.directory)
      << ", epoch " << r.epoch << "\n"
      << "inputs " << s.inputs << ", outputs " << s.outputs << ", data events " << s.data_events
      << ", timers " << s.timers << ", strategy errors " << s.strategy_errors << "\n"
      << "feed: messages " << r.feed.messages << ", events " << r.feed.events << ", decode errors "
      << r.feed.decode_errors << ", book syncs " << r.feed.book_syncs << "\n"
      << "venue: events " << r.venue.events << ", commands " << r.venue.commands << " (refused "
      << r.venue.refused_locally << ", unknown " << r.venue.unknown_outcomes << "), snapshots "
      << r.venue.snapshots << " (" << r.venue.snapshot_failures << " failed), countdowns "
      << r.venue.countdowns << " (" << r.venue.countdown_failures << " failed), decode errors "
      << r.venue.decode_errors << "\n";
  if (s.left_open != 0) {
    out << "left open at stop: " << s.left_open
        << " orders (the venue's countdownCancelAll, still armed, cancels them)\n";
  }
  for (const std::string& w : r.startup.warnings) {
    out << "warning: " << w << "\n";
  }
  return s.halted ? node::kExitFailed : node::kExitOk;
}

struct LiveRunner {
  template <strategy::StrategySet SS>
  int operator()(const std::string& program, const node::NodeArgs& parsed,
                 const node::NodeConfig& config, node::RunManifest manifest, SS& set) const {
    if (config.node.env == node::Env::Live) {
      return run_live_node(program, parsed, config, std::move(manifest), set);
    }
    if (config.node.env != node::Env::Sandbox) {
      return node::detail::BacktestRunner{}(program, parsed, config, std::move(manifest), set);
    }
    static std::atomic<bool> stop{false};
    const ShutdownSignals signals{stop};
    SandboxRequest request;
    request.config = &config;
    request.manifest = std::move(manifest);
    request.out = parsed.out;
    request.stop = &stop;
    if (parsed.run_for_s) {
      request.run_for = std::chrono::seconds{*parsed.run_for_s};
    }
    SandboxResult result;
    std::string error;
    node::NoHook hook;
    if (!core::ok(run_sandbox(request, set, result, error, hook))) {
      std::cerr << program << ": " << error << "\n";
      return node::kExitFailed;
    }
    return print_sandbox(std::cout, result);
  }

  template <strategy::StrategySet SS>
  static int run_live_node(const std::string& program, const node::NodeArgs& parsed,
                           const node::NodeConfig& config, node::RunManifest manifest, SS& set) {
    static std::atomic<bool> stop{false};
    const ShutdownSignals signals{stop};
    LiveRequest request;
    request.config = &config;
    request.manifest = std::move(manifest);
    request.out = parsed.out;
    request.stop = &stop;
    if (parsed.run_for_s) {
      request.run_for = std::chrono::seconds{*parsed.run_for_s};
    }
    LiveResult result;
    std::string error;
    node::NoHook hook;
    if (!core::ok(run_live(request, set, result, error, hook))) {
      std::cerr << program << ": " << error << "\n";
      return node::kExitFailed;
    }
    return print_live(std::cout, result);
  }
};

} // namespace live::detail

template <strategy::Strategy... S> int live_node_main(int argc, char** argv) {
  return node::detail::node_main_with<live::detail::LiveRunner, S...>(argc, argv, {});
}

} // namespace jarvis
