#pragma once

#include <cstdint>
#include <variant>

#include "jarvis/model/event.hpp"

// When a node takes an EngineState snapshot (docs/architecture.md section 16.3): right after
// stepping a BatchEnd input, once at least `every` inputs have been stepped since the last
// snapshot point (the first point counts from the start), or once an input asked for one
// (asks_for_snapshot). Taken there, the state is between two batches with every output flushed;
// replay applies the same rule to the recorded inputs, so both take their snapshots after the
// same inputs.

namespace jarvis::engine {

class SnapshotSchedule {
public:
  // `every` 0: never.
  explicit constexpr SnapshotSchedule(std::uint64_t every) noexcept : every_{every}, next_{every} {}

  // After the step of input `seq`; true when a snapshot is taken now.
  [[nodiscard]] constexpr bool due(std::uint64_t seq, bool batch_end) noexcept {
    if (!batch_end || (!requested_ && (every_ == 0 || seq < next_))) {
      return false;
    }
    next_ = seq + every_;
    requested_ = false;
    return true;
  }

  // An admin snapshot command: one at the next batch end.
  constexpr void request() noexcept { requested_ = true; }

  // Continues from a snapshot taken after input `seq`.
  constexpr void resume_after(std::uint64_t seq) noexcept { next_ = seq + every_; }

  [[nodiscard]] constexpr std::uint64_t every() const noexcept { return every_; }

private:
  std::uint64_t every_;
  std::uint64_t next_;
  bool requested_ = false;
};

// Whether the input just stepped asks for a snapshot at the next batch end: an admin snapshot
// command, or the Stopped transition of a node that stepped a Shutdown input (`shut_down`, the
// kernel's record of it): the final snapshot of a sandbox or live run (section 19.4), from which
// a resumed run continues without replaying anything. A backtest records no Shutdown.
[[nodiscard]] inline bool asks_for_snapshot(const model::Event& event, bool shut_down) noexcept {
  if (const auto* admin = std::get_if<model::AdminCommand>(&event)) {
    return admin->action == model::AdminAction::Snapshot;
  }
  if (const auto* lifecycle = std::get_if<model::NodeLifecycle>(&event)) {
    return shut_down && lifecycle->to == model::NodeState::Stopped;
  }
  return false;
}

} // namespace jarvis::engine
