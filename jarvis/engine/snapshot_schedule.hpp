#pragma once

#include <cstdint>

// When a node takes an EngineState snapshot (docs/architecture.md section 16.3): right after
// stepping a BatchEnd input, once at least `every` inputs have been stepped since the last
// snapshot point (the first point counts from the start). Taken there, the state is between two
// batches with every output flushed; replay applies the same rule to the recorded inputs, so
// both take their snapshots after the same inputs.

namespace jarvis::engine {

class SnapshotSchedule {
public:
  // `every` 0: never.
  explicit constexpr SnapshotSchedule(std::uint64_t every) noexcept : every_{every}, next_{every} {}

  // After the step of input `seq`; true when a snapshot is taken now.
  [[nodiscard]] constexpr bool due(std::uint64_t seq, bool batch_end) noexcept {
    if (every_ == 0 || !batch_end || seq < next_) {
      return false;
    }
    next_ = seq + every_;
    return true;
  }

  // Continues from a snapshot taken after input `seq`.
  constexpr void resume_after(std::uint64_t seq) noexcept { next_ = seq + every_; }

  [[nodiscard]] constexpr std::uint64_t every() const noexcept { return every_; }

private:
  std::uint64_t every_;
  std::uint64_t next_;
};

} // namespace jarvis::engine
