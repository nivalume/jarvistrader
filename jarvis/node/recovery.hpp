#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/node/snapshot_file.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// Crash recovery (docs/architecture.md section 16.3). A live node started with
// persistence.resume continues where the latest earlier run of this node stopped, whether it
// stopped cleanly or crashed:
//
//   1. the earlier run must have run the same configuration (config hash), and every strategy
//      must describe its own state (strategy.hpp), so that a snapshot restores it exactly;
//   2. the engine is restored from the earlier run's latest complete snapshot written by this
//      build, and the rest of its log is replayed on top with every output checked, as `--replay`
//      does. The log may end in a torn record, and the outputs of its last input may be missing
//      (a crash while they were written);
//   3. the new run starts from that state: its directory gets a snapshot of it under the earlier
//      run's last seq, its seq continues after that one, and its first input is RunStart with the
//      new epoch and that seq, which resets the session (connection health, reconciliation, the
//      lifecycle). A replay of the new run starts from that snapshot.
//
// What the earlier process had sent but not logged (async mode) is found by the reconciliation
// that every start runs; with persistence.mode = "barrier" nothing reached the venue that is not
// in the log.

namespace jarvis::node {

struct RecoveryReport {
  std::string from;               // the earlier run's directory; empty when nothing was resumed
  std::uint64_t snapshot_seq = 0; // the snapshot the state came from (0: the log's first input)
  std::uint64_t replayed = 0;     // inputs replayed after it
  std::uint64_t last_seq = 0;     // the earlier run's last input the state includes
  std::uint64_t torn_bytes = 0;   // a torn tail left out of its last segment
  bool torn_step = false;         // its last input's outputs or errors were not all logged
};

// Restores `engine`, built from `config` and not yet stepped, to the state at the end of the run
// log in `prior` (steps 1 and 2 above). A log without a single record leaves the engine as it
// is (last_seq 0): there is nothing to continue.
template <strategy::StrategySet SS>
[[nodiscard]] core::Status recover_run(const std::string& prior, const NodeConfig& config,
                                       engine::Engine<SS>& engine, RecoveryReport& report,
                                       std::string& error) {
  report = RecoveryReport{};
  report.from = prior;
  {
    EventLogReader probe;
    model::wire::RecordView first;
    core::Status s = probe.open(prior, EventLogReadOptions{true});
    if (core::ok(s)) {
      s = probe.next(first);
    }
    if (s == core::Status::EndOfStream) {
      return core::Status::Ok;
    }
    if (!core::ok(s)) {
      error = "cannot resume from " + prior +
              ": its log cannot be read: " + std::string{core::to_string(s)};
      return s;
    }
  }
  NodeConfig earlier;
  core::Status s = load_run_config(prior, earlier, error);
  if (!core::ok(s)) {
    error = "cannot resume: " + error;
    return s;
  }
  if (config_hash(earlier) != config_hash(config)) {
    error = "cannot resume from " + prior +
            ": it ran a different configuration (set persistence.resume = false to start afresh)";
    return core::Status::InvalidArgument;
  }
  if (!engine.snapshot_complete()) {
    error = "cannot resume: a strategy does not describe its own state (a C++ state(ar) member, "
            "or on_save and on_load in Python), so no snapshot can restore it";
    return core::Status::InvalidState;
  }
  ReplayOptions options;
  options.tolerate_torn_end = true;
  SnapshotEntry snapshot;
  if (pick_snapshot(prior, 0, SnapshotPick::Latest, snapshot)) {
    options.from_snapshot = snapshot.path;
  }
  ReplayReport replayed;
  s = replay_into(prior, config, engine, options, replayed, error);
  if (!core::ok(s)) {
    error = "cannot resume from " + prior + ": " + error;
    return s;
  }
  if (replayed.divergence) {
    error = "cannot resume from " + prior + ": its log does not replay at seq " +
            std::to_string(replayed.divergence->seq) + " (recorded " +
            replayed.divergence->recorded + ", replayed " + replayed.divergence->replayed + ")";
    return core::Status::InvalidState;
  }
  report.snapshot_seq = replayed.start_seq;
  report.replayed = replayed.inputs;
  report.last_seq = replayed.last_seq;
  report.torn_bytes = replayed.torn_bytes;
  report.torn_step = replayed.torn_step;
  return core::Status::Ok;
}

// The earlier run persistence.resume continues: the latest run directory of this node holding a
// run log, or none (empty).
[[nodiscard]] inline std::string resume_source(const NodeConfig& config) {
  const std::vector<std::string> runs = node_runs(config);
  return runs.empty() ? std::string{} : runs.back();
}

// Writes the resumed run's starting snapshot (step 3): the engine's state under `seq`, the
// earlier run's last input, in `directory`, synced.
template <strategy::StrategySet SS>
[[nodiscard]] core::Status
write_start_snapshot(const std::string& directory, const model::wire::LogHeader& header,
                     std::uint64_t seq, engine::Engine<SS>& engine, std::string& error) {
  std::vector<std::byte> body;
  core::Status s = engine.save_state(body);
  std::vector<std::byte> encoded;
  if (core::ok(s)) {
    const SnapshotInfo info{seq, engine.kernel().current.ts.value(), engine.snapshot_complete(),
                            static_cast<std::uint16_t>(engine.strategy_count())};
    s = encode_snapshot(header, info, body, encoded);
  }
  if (core::ok(s)) {
    s = write_snapshot_file(directory, seq, encoded, true);
  }
  if (!core::ok(s)) {
    error = "cannot write the starting snapshot in " + directory + ": " +
            std::string{core::to_string(s)};
  }
  return s;
}

} // namespace jarvis::node
