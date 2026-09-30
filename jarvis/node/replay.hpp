#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/engine/snapshot_schedule.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/snapshot_file.hpp"
#include "jarvis/node/state_text.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// Replay (docs/architecture.md sections 7.7 and 16.4): step the inputs of a run log into a fresh
// engine, recompute every output and compare it byte for byte with the recorded one. The first
// difference is a ReplayDivergence at that input's seq. Recorded StrategyError inputs of kind
// Exception must match the failures the replay reproduces; Overrun errors come from wall-clock
// measurements and are taken from the log as they are.
//
// EngineState snapshots (section 16.3): the replay takes a snapshot at the same points as the
// node (engine/snapshot_schedule.hpp, persistence.snapshot_every) and compares it with the
// snapshot file of that seq when the run directory has one; a different state is a divergence.
// With `from_snapshot` the replay starts from that snapshot file instead of the first input: the
// engine is restored from it and the log is replayed from the next input on.

namespace jarvis::node {

struct ReplayOptions {
  std::optional<std::uint64_t> until; // stop after the input with this seq and its outputs
  bool dump_state = false;
  bool check_snapshots = true;
  std::optional<std::string> from_snapshot; // a snapshot file of this run
};

struct Divergence {
  std::uint64_t seq = 0;
  std::string recorded; // what the log holds at that point
  std::string replayed; // what the replay computed instead
};

struct ReplayReport {
  std::uint64_t inputs = 0;
  std::uint64_t outputs = 0;
  std::optional<Divergence> divergence;
  std::string state;                   // kernel_state_text after the last step, when dump_state
  std::uint64_t snapshots_checked = 0; // snapshot files that matched the replayed state
  std::uint64_t start_seq = 0;         // with from_snapshot: the snapshot's seq
};

namespace detail {

struct KeyedOutput {
  core::EventKey key;
  model::Output output;
};

struct OutputBuffer {
  std::vector<KeyedOutput> items;
  core::Status emit(const core::EventKey& key, const model::Output& output) {
    items.push_back(KeyedOutput{key, output});
    return core::Status::Ok;
  }
};

[[nodiscard]] std::string input_line(const model::wire::RecordHeader& header,
                                     const model::Event& event);
[[nodiscard]] std::string output_line(const core::EventKey& key, const model::Output& output);
[[nodiscard]] bool same_record(const core::EventKey& key, const model::Output& output,
                               std::span<const std::byte> recorded);
[[nodiscard]] std::string failure_text(const strategy::StrategyFailure& failure);

// Compares recorded StrategyError inputs against the failures the replay reproduced.
class ErrorCheck {
public:
  // Empty when `event` is acceptable at this point, else what the replay expected instead.
  [[nodiscard]] std::optional<std::string> check(const model::Event& event) {
    if (const auto* e = std::get_if<model::StrategyError>(&event)) {
      if (e->kind == model::StrategyErrorKind::Overrun) {
        return std::nullopt;
      }
      if (expected_.empty()) {
        return std::string{"no strategy failure at this point"};
      }
      const strategy::StrategyFailure f = expected_.front();
      if (f.strategy != e->strategy_index || f.message_hash != e->message_hash) {
        return failure_text(f);
      }
      expected_.erase(expected_.begin());
      return std::nullopt;
    }
    if (!expected_.empty()) {
      return failure_text(expected_.front());
    }
    return std::nullopt;
  }
  void add(const strategy::StrategyFailure& f) { expected_.push_back(f); }
  [[nodiscard]] bool empty() const noexcept { return expected_.empty(); }
  [[nodiscard]] std::string pending() const { return failure_text(expected_.front()); }

private:
  std::vector<strategy::StrategyFailure> expected_;
};

} // namespace detail

namespace detail {

// One replay pass over a run log; see replay_run.
template <strategy::StrategySet SS> class Replayer {
public:
  Replayer(const NodeConfig& config, SS& strategies, const ReplayOptions& options,
           ReplayReport& report, std::string directory)
      : engine_{kernel_config(config), strategies, error_policy(config)}, options_{&options},
        report_{&report}, directory_{std::move(directory)},
        snapshots_{config.persistence.snapshot_every} {
    name_strategies(config, engine_.kernel());
  }

  // Restores the engine from a snapshot file of this run; the replay then starts after its seq.
  [[nodiscard]] core::Status restore(const std::string& path, std::string& error) {
    std::vector<std::byte> file;
    core::Status s = read_file_bytes(path, file);
    model::wire::LogHeader header;
    SnapshotInfo info;
    std::span<const std::byte> body;
    if (core::ok(s)) {
      s = decode_snapshot(file, header, info, body);
    }
    if (core::ok(s)) {
      s = engine_.load_state(body);
    }
    if (!core::ok(s)) {
      error = path + ": cannot restore the snapshot: " + std::string{core::to_string(s)};
      return s;
    }
    if (!info.complete) {
      error = path + ": the snapshot lacks the state of a strategy that does not describe it";
      return core::Status::InvalidState;
    }
    lifecycle_.restore(engine_.kernel().node_state);
    snapshots_.resume_after(info.seq);
    skip_until_ = info.seq;
    last_seq_ = info.seq;
    report_->start_seq = info.seq;
    return core::Status::Ok;
  }

  [[nodiscard]] core::Status run(EventLogReader& reader, std::string& error) {
    model::wire::RecordView record;
    while (!report_->divergence && !stopped_) {
      const core::Status s = reader.next(record);
      if (s == core::Status::EndOfStream) {
        finish();
        break;
      }
      if (!core::ok(s)) {
        error = "unreadable record after seq " + std::to_string(last_seq_) + ": " +
                std::string{core::to_string(s)};
        return s;
      }
      if (record.header.seq <= skip_until_) {
        continue; // before the snapshot the replay started from
      }
      const core::Status handled = record.header.kind >= model::wire::kFirstOutputKind
                                       ? on_output(record, error)
                                       : on_input(reader, record, error);
      if (!core::ok(handled)) {
        return handled;
      }
    }
    if (options_->dump_state) {
      report_->state = kernel_state_text(engine_.kernel());
    }
    return core::Status::Ok;
  }

private:
  void diverge(std::uint64_t seq, std::string recorded, std::string replayed) {
    report_->divergence = Divergence{seq, std::move(recorded), std::move(replayed)};
  }

  [[nodiscard]] std::string next_computed() const {
    return output_line(computed_.items[matched_].key, computed_.items[matched_].output);
  }

  core::Status on_output(const model::wire::RecordView& record, std::string& error) {
    model::Output logged;
    const core::Status s = EventLogReader::decode_output(record, logged);
    if (!core::ok(s)) {
      error = "undecodable output record at seq " + std::to_string(record.header.seq);
      return s;
    }
    const core::EventKey key{record.header.ts, record.header.source_id, record.header.seq};
    if (matched_ >= computed_.items.size()) {
      diverge(record.header.seq, output_line(key, logged), "no output");
    } else if (!same_record(computed_.items[matched_].key, computed_.items[matched_].output,
                            record.bytes)) {
      diverge(record.header.seq, output_line(key, logged), next_computed());
    } else {
      ++matched_;
      ++report_->outputs;
    }
    return core::Status::Ok;
  }

  core::Status on_input(EventLogReader& reader, const model::wire::RecordView& record,
                        std::string& error) {
    if (matched_ < computed_.items.size()) {
      diverge(last_seq_, "no output", next_computed());
      return core::Status::Ok;
    }
    if (options_->until && record.header.seq > *options_->until) {
      stopped_ = true;
      return core::Status::Ok;
    }
    model::Event event;
    const core::Status s = reader.decode(record, event);
    if (!core::ok(s)) {
      error = "undecodable input record at seq " + std::to_string(record.header.seq);
      return s;
    }
    if (const std::optional<std::string> refused = admit(event)) {
      diverge(record.header.seq, input_line(record.header, event), *refused);
      return core::Status::Ok;
    }
    step(record, event);
    return core::Status::Ok;
  }

  // Why the replay cannot accept `event` here, if it cannot.
  std::optional<std::string> admit(const model::Event& event) {
    if (std::optional<std::string> expected = errors_.check(event)) {
      return expected;
    }
    if (const auto* lc = std::get_if<model::NodeLifecycle>(&event)) {
      const model::NodeState from = lifecycle_.state();
      if (!core::ok(lifecycle_.replay(*lc))) {
        return "a transition the lifecycle does not allow from " +
               std::string{model::to_string(from)};
      }
    }
    if (std::holds_alternative<model::RunStart>(event)) {
      lifecycle_.restore(model::NodeState::Init); // a continued run's lifecycle starts over
    }
    return std::nullopt;
  }

  void step(const model::wire::RecordView& record, const model::Event& event) {
    const core::EventKey key{record.header.ts, record.header.source_id, record.header.seq};
    const core::Status s = engine_.step(key, event);
    if (!core::ok(s)) {
      diverge(record.header.seq, input_line(record.header, event),
              "the step failed: " + std::string{core::to_string(s)});
      return;
    }
    computed_.items.clear();
    matched_ = 0;
    static_cast<void>(engine_.flush_outputs(computed_));
    if (!std::holds_alternative<model::StrategyError>(event)) {
      for (const strategy::StrategyFailure& f : engine_.failures()) {
        errors_.add(f);
      }
    }
    engine_.clear_failures();
    last_seq_ = record.header.seq;
    ++report_->inputs;
    if (snapshots_.due(record.header.seq, std::holds_alternative<model::BatchEnd>(event))) {
      check_snapshot(record.header.seq);
    }
  }

  // The state here against the node's snapshot file of this seq, if the directory has it.
  void check_snapshot(std::uint64_t seq) {
    if (!options_->check_snapshots || directory_.empty()) {
      return;
    }
    std::vector<std::byte> file;
    if (!core::ok(read_file_bytes(directory_ + "/" + snapshot_name(seq), file))) {
      return; // not written (or removed): nothing to compare
    }
    model::wire::LogHeader header;
    SnapshotInfo info;
    std::span<const std::byte> body;
    if (!core::ok(decode_snapshot(file, header, info, body))) {
      diverge(seq, "an unreadable snapshot file " + snapshot_name(seq), "a snapshot");
      return;
    }
    std::vector<std::byte> state;
    if (!core::ok(engine_.save_state(state))) {
      diverge(seq, "a snapshot", "no snapshot: saving the state failed");
      return;
    }
    if (state.size() != body.size() || !std::equal(state.begin(), state.end(), body.begin())) {
      std::size_t at = 0;
      while (at < state.size() && at < body.size() && state[at] == body[at]) {
        ++at;
      }
      diverge(seq,
              "snapshot " + snapshot_name(seq) + " (" + std::to_string(body.size()) + " bytes)",
              "a state differing from byte " + std::to_string(at) + " (" +
                  std::to_string(state.size()) + " bytes)");
      return;
    }
    ++report_->snapshots_checked;
  }

  // End of the log: every computed output and failure must have been recorded.
  void finish() {
    if (report_->divergence) {
      return;
    }
    if (matched_ < computed_.items.size()) {
      diverge(last_seq_, "no output", next_computed());
    } else if (!errors_.empty()) {
      diverge(last_seq_, "the end of the log", errors_.pending());
    }
  }

  engine::Engine<SS> engine_;
  engine::Lifecycle lifecycle_;
  const ReplayOptions* options_;
  ReplayReport* report_;
  std::string directory_;
  engine::SnapshotSchedule snapshots_;
  std::uint64_t skip_until_ = 0;
  OutputBuffer computed_;
  ErrorCheck errors_;
  std::size_t matched_ = 0;
  std::uint64_t last_seq_ = 0;
  bool stopped_ = false;
};

} // namespace detail

template <strategy::StrategySet SS>
[[nodiscard]] core::Status replay_run(const std::string& directory, const NodeConfig& config,
                                      SS& strategies, const ReplayOptions& options,
                                      ReplayReport& report, std::string& error) {
  report = ReplayReport{};
  EventLogReader reader;
  const core::Status s = reader.open(directory);
  if (!core::ok(s)) {
    error = directory + ": cannot open the run log: " + std::string{core::to_string(s)};
    return s;
  }
  detail::Replayer<SS> replayer{config, strategies, options, report, directory};
  if (options.from_snapshot) {
    const core::Status restored = replayer.restore(*options.from_snapshot, error);
    if (!core::ok(restored)) {
      return restored;
    }
  }
  const core::Status r = replayer.run(reader, error);
  if (!core::ok(r)) {
    error = directory + ": " + error;
  }
  return r;
}

} // namespace jarvis::node
