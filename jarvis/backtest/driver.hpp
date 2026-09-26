#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <variant>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// The backtest step loop (docs/architecture.md sections 4.3 and 5.4). It turns a stream of data
// events into the node's input sequence and steps the engine through it:
//
//   - lifecycle: Init -> Wired -> Starting -> Syncing -> Running before the first data event,
//     Running -> Stopping -> Stopped after the last one (or when a strategy error halts the node);
//   - timers: before each data event, every timer due at or before its ts fires as TimerFired;
//   - batches: a batch is a maximal run of inputs with the same ts, closed by BatchEnd;
//   - strategy failures: each one the engine reports becomes a StrategyError input right after
//     the step that caused it. A failure inside on_error itself is not converted again.
//
// Every input the driver steps is recorded first, with the key (ts, source_id, seq): seq is the
// driver's own counter from 1, source_id is the data source's id, or kKernelSource for the
// inputs the driver synthesises. The engine's outputs follow their input. The recording is
// therefore a complete run log: replaying it reproduces every call and output without this
// driver (the same holds for sandbox and live, whose ingestion order is the recording).

namespace jarvis::backtest {

inline constexpr std::uint16_t kKernelSource = 0;

// Receives every input before it is stepped (`record`) and every output after (`emit`).
template <typename R>
concept Recorder =
    engine::CommandSink<R> && requires(R& r, const core::EventKey& key, const model::Event& event) {
      { r.record(key, event) } -> std::same_as<core::Status>;
    };

struct DriverOptions {
  std::optional<core::UnixNanos> start; // data before start is skipped
  std::optional<core::UnixNanos> end;   // data at or after end is not stepped; timers due
                                        // before end still fire
};

struct RunSummary {
  std::uint64_t data_events = 0; // data events stepped
  std::uint64_t skipped = 0;     // data events outside [start, end)
  std::uint64_t inputs = 0;      // every recorded input, synthesised ones included
  std::uint64_t outputs = 0;
  std::uint64_t batches = 0;
  std::uint64_t timers = 0;
  std::uint64_t strategy_errors = 0;
  bool halted = false; // a strategy error stopped the node (ErrorPolicy::HaltNode)
  model::NodeState state = model::NodeState::Init;
  core::UnixNanos first_ts;
  core::UnixNanos last_ts;
};

template <strategy::StrategySet SS, engine::EventSource Source, Recorder Rec> class Driver {
public:
  Driver(engine::Engine<SS>& engine, Source& source, Rec& recorder, DriverOptions options = {})
      : engine_{&engine}, source_{&source}, recorder_{&recorder}, options_{options},
        failures_{engine.kernel().failures.capacity()} {}

  [[nodiscard]] core::Status run(RunSummary& out) {
    summary_ = RunSummary{};
    core::EventKey key;
    model::Event event;
    core::Status s = next_data(key, event);
    bool have = core::ok(s);
    if (!have && s != core::Status::EndOfStream) {
      return s;
    }
    const core::UnixNanos ts0 = options_.start.value_or(have ? key.ts : core::UnixNanos{});
    summary_.first_ts = ts0;
    for (const auto reason :
         {model::LifecycleReason::Configured, model::LifecycleReason::RunRequested,
          model::LifecycleReason::Started, model::LifecycleReason::Synced}) {
      s = transition(reason, ts0);
      if (!core::ok(s)) {
        return s;
      }
    }
    while (have && !engine_->halt_requested()) {
      s = fire_timers(key.ts, true);
      if (!core::ok(s)) {
        return s;
      }
      if (engine_->halt_requested()) {
        break;
      }
      s = feed(key, event);
      if (!core::ok(s)) {
        return s;
      }
      ++summary_.data_events;
      s = next_data(key, event);
      have = core::ok(s);
      if (!have && s != core::Status::EndOfStream) {
        return s;
      }
    }
    summary_.halted = engine_->halt_requested();
    if (!summary_.halted && options_.end) {
      s = fire_timers(*options_.end, false);
      if (!core::ok(s)) {
        return s;
      }
      summary_.halted = engine_->halt_requested();
    }
    const core::UnixNanos final_ts = last_ts_;
    s = transition(summary_.halted ? model::LifecycleReason::ShutdownRequested
                                   : model::LifecycleReason::EndOfData,
                   final_ts);
    if (!core::ok(s)) {
      return s;
    }
    s = transition(model::LifecycleReason::Drained, final_ts);
    if (!core::ok(s)) {
      return s;
    }
    s = close_batch();
    if (!core::ok(s)) {
      return s;
    }
    summary_.state = lifecycle_.state();
    summary_.last_ts = last_ts_;
    out = summary_;
    return core::Status::Ok;
  }

private:
  // The next data event inside [start, end); EndOfStream past the end.
  core::Status next_data(core::EventKey& key, model::Event& event) {
    while (true) {
      const core::Status s = source_->next(key, event);
      if (!core::ok(s)) {
        return s;
      }
      if (options_.end && !(key.ts < *options_.end)) {
        ++summary_.skipped;
        return core::Status::EndOfStream;
      }
      if (options_.start && key.ts < *options_.start) {
        ++summary_.skipped;
        continue;
      }
      return core::Status::Ok;
    }
  }

  core::Status transition(model::LifecycleReason reason, core::UnixNanos ts) {
    model::NodeLifecycle event;
    const core::Status s = lifecycle_.apply(reason, ts, event);
    if (!core::ok(s)) {
      return s;
    }
    return feed(core::EventKey{ts, kKernelSource, 0}, model::Event{event});
  }

  // Fires every timer due at `limit` (inclusive) or before it (exclusive).
  core::Status fire_timers(core::UnixNanos limit, bool inclusive) {
    core::FiredTimer due;
    while (!engine_->halt_requested() && engine_->next_timer(due) &&
           (due.deadline < limit || (inclusive && due.deadline == limit))) {
      const core::UnixNanos ts = due.deadline < last_ts_ ? last_ts_ : due.deadline;
      const core::Status s = feed(core::EventKey{ts, kKernelSource, 0},
                                  model::Event{model::TimerFired{due.key, due.deadline, ts}});
      if (!core::ok(s)) {
        return s;
      }
      ++summary_.timers;
    }
    return core::Status::Ok;
  }

  // Steps one input, closing the open batch first when the input starts a new one.
  core::Status feed(core::EventKey key, const model::Event& event) {
    if (batch_open_ && key.ts != batch_ts_) {
      const core::Status s = close_batch();
      if (!core::ok(s)) {
        return s;
      }
    }
    batch_open_ = true;
    batch_ts_ = key.ts;
    return step_input(key, event);
  }

  core::Status close_batch() {
    if (!batch_open_) {
      return core::Status::Ok;
    }
    batch_open_ = false;
    ++summary_.batches;
    return step_input(core::EventKey{batch_ts_, kKernelSource, 0},
                      model::Event{model::BatchEnd{summary_.batches, batch_ts_}});
  }

  core::Status step_input(core::EventKey key, const model::Event& event) {
    key.seq = ++seq_;
    core::Status s = recorder_->record(key, event);
    if (!core::ok(s)) {
      return s;
    }
    ++summary_.inputs;
    last_ts_ = key.ts;
    s = engine_->step(key, event);
    if (!core::ok(s)) {
      return s;
    }
    summary_.outputs += engine_->outputs().size();
    s = engine_->flush_outputs(*recorder_);
    if (!core::ok(s)) {
      return s;
    }
    if (std::holds_alternative<model::StrategyError>(event)) {
      engine_->clear_failures(); // on_error failed: the policy has already been applied
      return core::Status::Ok;
    }
    // The StrategyError steps below share this input's ts, so they never close a batch and
    // never reach this point again: failures_ is stable while the loop runs.
    failures_.clear();
    for (const strategy::StrategyFailure& f : engine_->failures()) {
      static_cast<void>(failures_.push_back(f));
    }
    engine_->clear_failures();
    for (std::size_t i = 0; i < failures_.size(); ++i) {
      const strategy::StrategyFailure f = failures_[i];
      ++summary_.strategy_errors;
      s = feed(core::EventKey{key.ts, kKernelSource, 0},
               model::Event{model::StrategyError{f.strategy, f.kind, f.message_hash, key.ts}});
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  engine::Engine<SS>* engine_;
  Source* source_;
  Rec* recorder_;
  DriverOptions options_;
  engine::Lifecycle lifecycle_;
  core::FixedVector<strategy::StrategyFailure> failures_;
  RunSummary summary_;
  std::uint64_t seq_ = 0;
  bool batch_open_ = false;
  core::UnixNanos batch_ts_;
  core::UnixNanos last_ts_;
};

} // namespace jarvis::backtest
