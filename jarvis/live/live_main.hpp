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

inline void print_persist(std::ostream& out, const PersistStats& p) {
  if (p.records == 0) {
    return;
  }
  out << "log: records " << p.records << ", bytes " << p.position << ", durable " << p.durable
      << ", syncs " << p.syncs << ", segments " << p.segments << ", largest sync " << p.max_lag
      << " bytes, ring stalls " << p.stalls << "\n";
  if (p.snapshots != 0 || p.snapshots_dropped != 0) {
    out << "snapshots: " << p.snapshots << " written, " << p.snapshots_dropped
        << " replaced while waiting, " << p.segments_removed << " log segments removed\n";
  }
}

inline void print_telemetry(std::ostream& out, std::uint64_t lines, std::uint64_t dropped) {
  if (lines != 0 || dropped != 0) {
    out << "telemetry: " << lines << " JSON lines, " << dropped << " records dropped\n";
  }
}

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
  print_persist(out, r.persist);
  print_telemetry(out, r.telemetry_lines, r.telemetry_dropped);
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
      << r.venue.decode_errors << ", waited for the log " << r.venue.barrier_waits << "\n";
  if (!r.recovery.from.empty()) {
    const node::RecoveryReport& c = r.recovery;
    out << "resumed from " << c.from << " at seq " << c.last_seq << " (snapshot " << c.snapshot_seq
        << ", " << c.replayed << " inputs replayed";
    if (c.torn_bytes != 0) {
      out << ", a torn tail of " << c.torn_bytes << " bytes left out";
    }
    if (c.torn_step) {
      out << ", the last input's outputs incomplete";
    }
    out << ")\n";
  }
  print_persist(out, r.persist);
  print_telemetry(out, r.telemetry_lines, r.telemetry_dropped);
  if (r.venue.unsent_at_stop != 0) {
    out << "not sent at stop: " << r.venue.unsent_at_stop
        << " commands (their records never became durable)\n";
  }
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
