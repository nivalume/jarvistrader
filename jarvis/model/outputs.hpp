#pragma once

#include <cstdint>
#include <variant>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/fixed_point.hpp"

// Kernel outputs (docs/architecture.md section 16.1): what `step` produces, written to the event
// log after the input that caused it. Replay recomputes them from the inputs and compares them
// byte for byte (ReplayDivergence). Order commands join this variant in M3.

namespace jarvis::model {

using FeatureId = std::uint32_t;

// A kernel feature value delivered to at least one subscriber.
struct FeatureUpdate {
  FeatureId feature_id = 0;
  Decimal value;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
};

using RecordTag = core::FixedString<32>;

// A value a strategy recorded with ctx.record(tag, value): its deterministic, replay-checked
// output when it does not trade.
struct StrategyRecord {
  std::uint16_t strategy_index = 0;
  RecordTag tag;
  Decimal value;
  core::UnixNanos ts_init;
};

using Output = std::variant<FeatureUpdate, StrategyRecord>;

} // namespace jarvis::model
