#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/engine/snapshot_schedule.hpp"
#include "jarvis/engine/sync_gate.hpp"
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
// After every input the driver applies the sync gate (engine/sync_gate.hpp): the account's
// reconciliation phase moves the node between Syncing, Running and Degraded. With
// DriverOptions::await_sync (live) the node stays Syncing after the preamble until the account
// is synced; otherwise it enters Running at once, as it always has.
//
// With a venue loop as the source (VenueLoop, section 12.2), the driver also runs the simulated
// venue: it always processes the earliest of the next venue-side event (market data the venue
// matches on, a command arriving), the next timer and the next kernel input (delayed market
// data, a venue answer), in that order on a tie, and hands the commands of every step to the
// venue loop.
//
// EngineState snapshots (DriverOptions::snapshot_every, section 16.3): at each point of the
// SnapshotSchedule the driver hands the engine to the recorder's snapshot(engine, key), when the
// recorder has one; the key is the BatchEnd input's. A snapshot that fails (a strategy's own
// state that cannot be saved, a file that cannot be written) is counted, and the run goes on:
// the log, not the snapshot, is the record.
//
// Every input the driver steps is recorded first, with the key (ts, source_id, seq): seq is the
// driver's own counter from 1, source_id is the data source's id, or kKernelSource for the
// inputs the driver synthesises. The engine's outputs follow their input. The recording is
// therefore a complete run log: replaying it reproduces every call and output without this
// driver (the same holds for sandbox and live, whose ingestion order is the recording).

namespace jarvis::backtest {

inline constexpr std::uint16_t kKernelSource = 0;
// source_id of the simulated venue's answers (the same value as backtest::kVenueSource).
inline constexpr std::uint16_t kVenueSourceId = 0xFFFE;

// Receives every input before it is stepped (`record`) and every output after (`emit`).
// A source that also runs a simulated venue (VenueLoop).
template <typename S>
concept VenueSource =
    requires(S& s, std::optional<core::UnixNanos>& t, core::EventKey& key, model::Event& event,
             core::UnixNanos now, std::span<const model::Output> outputs) {
      { s.next_venue_time(t) } -> std::same_as<core::Status>;
      { s.next_input_time() } -> std::same_as<std::optional<core::UnixNanos>>;
      { s.process_venue() } -> std::same_as<core::Status>;
      { s.pop_input(key, event) } -> std::same_as<bool>;
      { s.on_outputs(now, outputs) } -> std::same_as<core::Status>;
    };

template <typename R>
concept Recorder =
    engine::CommandSink<R> && requires(R& r, const core::EventKey& key, const model::Event& event) {
      { r.record(key, event) } -> std::same_as<core::Status>;
    };

// Real time (sandbox, live): the clock, and the hand-off of what arrived since the last call.
// `pump` moves arrived inputs into the source, stamped at `now`; the source returns WouldBlock
// when it has nothing. `idle` runs when nothing is due (yield, Python's on_idle hook).
template <typename P>
concept Pump = requires(P& p, const P& cp, core::UnixNanos now) {
  { cp.now() } -> std::same_as<core::UnixNanos>;
  { p.pump(now) } -> std::same_as<core::Status>;
  { p.idle(now) } -> std::same_as<core::Status>;
  { cp.stop_requested() } -> std::same_as<bool>;
};

struct DriverOptions {
  std::optional<core::UnixNanos> start; // data before start is skipped
  std::optional<core::UnixNanos> end;   // data at or after end is not stepped; timers due
                                        // before end still fire
  // Inputs stepped while the node is Syncing (instrument definitions, the account snapshot):
  // what reconciliation provides in live, before strategies start.
  std::span<const model::Event> preamble;
  // Live: Running waits until the account is reconciled with the venue (section 15.1).
  bool await_sync = false;
  // Real time: how the node stops (section 19.4). A Shutdown input with this mode comes before
  // Stopping; with CancelAllThenExit the node then stays in Stopping, still stepping inputs,
  // until no order is open or `drain_for` has passed.
  std::optional<model::ShutdownMode> shutdown;
  core::DurationNanos drain_for{10'000'000'000};
  std::uint64_t snapshot_every = 0; // persistence.snapshot_every; 0: no snapshots
  // Live (sections 4.1 and 16.3): the RunStart input, stamped at the start, is the first one,
  // before the lifecycle starts; seq continues after its prior_seq (a run that continues an
  // earlier run whose state the engine was restored to).
  std::optional<model::RunStart> run_start;
};

struct RunSummary {
  std::uint64_t data_events = 0; // data events stepped
  std::uint64_t skipped = 0;     // data events outside [start, end)
  std::uint64_t inputs = 0;      // every recorded input, synthesised ones included
  std::uint64_t outputs = 0;
  std::uint64_t batches = 0;
  std::uint64_t timers = 0;
  std::uint64_t strategy_errors = 0;
  std::uint64_t venue_answers = 0; // order events from the simulated venue
  bool halted = false;             // a strategy error stopped the node (ErrorPolicy::HaltNode)
  std::uint32_t left_open = 0;     // orders still open when the node stopped
  std::uint64_t snapshots = 0;     // EngineState snapshots taken
  std::uint64_t snapshot_failures = 0;
  model::NodeState state = model::NodeState::Init;
  core::UnixNanos first_ts;
  core::UnixNanos last_ts;
};

template <strategy::StrategySet SS, typename Source, Recorder Rec>
  requires engine::EventSource<Source> || VenueSource<Source>
class Driver {
public:
  Driver(engine::Engine<SS>& engine, Source& source, Rec& recorder, DriverOptions options = {})
      : engine_{&engine}, source_{&source}, recorder_{&recorder}, options_{options},
        failures_{engine.kernel().failures.capacity()}, snapshots_{options.snapshot_every} {}

  [[nodiscard]] core::Status run(RunSummary& out) {
    summary_ = RunSummary{};
    core::Status s = core::Status::Ok;
    if constexpr (VenueSource<Source>) {
      s = run_venue();
    } else {
      s = run_plain();
    }
    if (!core::ok(s)) {
      return s;
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

  // The same input sequence in real time: an input is stepped once the clock has reached its
  // time, and every input the pump delivers is stamped with the clock's reading, so the log is
  // a backtest input sequence that happened to arrive in real time and replays the same way.
  // A batch closes when the next input has a later ts or when nothing more is due. The run
  // ends when the pump asks to stop (ShutdownRequested), an admin shutdown was stepped, or a
  // strategy error halts the node.
  template <Pump P> [[nodiscard]] core::Status run_realtime(P& pump, RunSummary& out) {
    summary_ = RunSummary{};
    core::Status s = start(pump.now());
    while (core::ok(s) && !engine_->halt_requested() && !engine_->stop_requested() &&
           !pump.stop_requested()) {
      s = realtime_round(pump);
    }
    if (!core::ok(s)) {
      return s;
    }
    summary_.halted = engine_->halt_requested();
    s = shut_down(pump);
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
  [[nodiscard]] bool halted() const noexcept { return !draining_ && engine_->halt_requested(); }

  // One round of the real-time loop: what the pump delivers, then everything due.
  template <Pump P> core::Status realtime_round(P& pump) {
    const core::UnixNanos now = pump.now();
    core::Status s = pump.pump(now);
    if (!core::ok(s)) {
      return s;
    }
    bool moved = false;
    s = advance_until(now, moved);
    if (core::ok(s) && !moved) {
      s = close_batch();
      if (core::ok(s)) {
        s = pump.idle(now);
      }
    }
    return s;
  }

  // Shutdown (with DriverOptions::shutdown), Stopping, the drain, Stopped. The drain steps
  // inputs even after a halt: the venue's answers to the cancels must still reach the kernel.
  template <Pump P> core::Status shut_down(P& pump) {
    const auto clock = [this, &pump] {
      const core::UnixNanos now = pump.now();
      return now < last_ts_ ? last_ts_ : now;
    };
    core::UnixNanos now = clock();
    core::Status s = core::Status::Ok;
    if (options_.shutdown) {
      s = feed(core::EventKey{now, kKernelSource, 0},
               model::Event{model::Shutdown{*options_.shutdown, now}});
    }
    if (core::ok(s)) {
      s = transition(model::LifecycleReason::ShutdownRequested, now);
    }
    if (core::ok(s) && options_.shutdown == model::ShutdownMode::CancelAllThenExit) {
      const core::UnixNanos until{now.value() + options_.drain_for.value()};
      draining_ = true;
      while (core::ok(s) && engine_->open_orders() > 0 && pump.now() < until) {
        s = realtime_round(pump);
      }
      draining_ = false;
    }
    if (!core::ok(s)) {
      return s;
    }
    summary_.left_open = engine_->open_orders();
    return transition(model::LifecycleReason::Drained, clock());
  }

  // Steps everything due at `now` or before, earliest first.
  core::Status advance_until(core::UnixNanos now, bool& moved) {
    if constexpr (VenueSource<Source>) {
      return advance_venue_until(now, moved);
    } else {
      return advance_plain_until(now, moved);
    }
  }

  core::Status advance_venue_until(core::UnixNanos now, bool& moved) {
    const auto due_at = [now](const std::optional<core::UnixNanos>& t) {
      return t && !(now < *t) ? t : std::nullopt;
    };
    while (!halted()) {
      std::optional<core::UnixNanos> venue;
      const core::Status s = source_->next_venue_time(venue);
      if (!core::ok(s)) {
        return s;
      }
      const std::optional<core::UnixNanos> input = source_->next_input_time();
      core::FiredTimer due;
      const bool timer = engine_->next_timer(due) && !(now < due.deadline);
      if (!due_at(venue) && !due_at(input) && !timer) {
        return core::Status::Ok;
      }
      // advance() takes the earliest of what is due.
      const core::Status a = advance(due_at(venue), due_at(input));
      if (!core::ok(a)) {
        return a;
      }
      moved = true;
    }
    return core::Status::Ok;
  }

  core::Status advance_plain_until(core::UnixNanos now, bool& moved) {
    while (!halted()) {
      core::EventKey key;
      model::Event event;
      const core::Status s = source_->next(key, event);
      if (s == core::Status::WouldBlock || s == core::Status::EndOfStream) {
        return fire_timers(now, true, &moved);
      }
      if (!core::ok(s)) {
        return s;
      }
      core::Status f = fire_timers(key.ts, true, &moved);
      if (core::ok(f)) {
        f = feed(key, event);
        ++summary_.data_events;
        moved = true;
      }
      if (!core::ok(f)) {
        return f;
      }
    }
    return core::Status::Ok;
  }

  // Init -> Running at ts0, with the preamble stepped while Syncing.
  core::Status start(core::UnixNanos ts0) {
    summary_.first_ts = ts0;
    if (options_.run_start) {
      seq_ = options_.run_start->prior_seq;
      snapshots_.resume_after(seq_);
      model::RunStart run_start = *options_.run_start;
      run_start.ts_init = ts0;
      const core::Status s = feed(core::EventKey{ts0, kKernelSource, 0}, model::Event{run_start});
      if (!core::ok(s)) {
        return s;
      }
    }
    for (const auto reason :
         {model::LifecycleReason::Configured, model::LifecycleReason::RunRequested,
          model::LifecycleReason::Started}) {
      const core::Status s = transition(reason, ts0);
      if (!core::ok(s)) {
        return s;
      }
    }
    for (const model::Event& e : options_.preamble) {
      const core::Status s = feed(core::EventKey{ts0, kKernelSource, 0}, e);
      if (!core::ok(s)) {
        return s;
      }
    }
    gating_ = true;
    return options_.await_sync ? gate(ts0) : transition(model::LifecycleReason::Synced, ts0);
  }

  // Applies the sync gate until it calls for nothing more.
  core::Status gate(core::UnixNanos ts) {
    gate_active_ = true;
    core::Status s = core::Status::Ok;
    while (core::ok(s)) {
      const std::optional<model::LifecycleReason> move =
          engine::sync_move(lifecycle_.state(), engine_->kernel().trading.reconciler.phase(),
                            options_.await_sync, engine_->kernel().health);
      if (!move) {
        break;
      }
      s = transition(*move, ts);
    }
    gate_active_ = false;
    return s;
  }

  core::Status run_plain() {
    core::EventKey key;
    model::Event event;
    core::Status s = next_data(key, event);
    bool have = core::ok(s);
    if (!have && s != core::Status::EndOfStream) {
      return s;
    }
    s = start(options_.start.value_or(have ? key.ts : core::UnixNanos{}));
    if (!core::ok(s)) {
      return s;
    }
    while (have && !halted()) {
      s = fire_timers(key.ts, true);
      if (!core::ok(s)) {
        return s;
      }
      if (halted()) {
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
    return core::Status::Ok;
  }

  // The earliest of venue side, timer and kernel input, in that order on a tie.
  core::Status run_venue() {
    std::optional<core::UnixNanos> venue;
    core::Status s = source_->next_venue_time(venue);
    if (!core::ok(s)) {
      return s;
    }
    s = start(options_.start.value_or(venue.value_or(core::UnixNanos{})));
    if (!core::ok(s)) {
      return s;
    }
    while (!halted()) {
      s = source_->next_venue_time(venue);
      if (!core::ok(s)) {
        return s;
      }
      const std::optional<core::UnixNanos> input = source_->next_input_time();
      if (!venue && !input) {
        break; // timers after the last event fire up to data.range's end, as without a venue
      }
      s = advance(venue, input);
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  // One venue-mode move: the venue side, a timer or a kernel input, whichever is earliest.
  core::Status advance(const std::optional<core::UnixNanos>& venue,
                       const std::optional<core::UnixNanos>& input) {
    core::FiredTimer due;
    const bool timer = engine_->next_timer(due);
    if (venue && (!timer || !(due.deadline < *venue)) && (!input || !(*input < *venue))) {
      return source_->process_venue();
    }
    if (timer && (!input || !(*input < due.deadline))) {
      return fire_timers(due.deadline, true);
    }
    core::EventKey key;
    model::Event event;
    if (!source_->pop_input(key, event)) {
      return core::Status::Ok;
    }
    (key.source_id == kVenueSourceId ? summary_.venue_answers : summary_.data_events) += 1;
    return feed(key, event);
  }

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
  core::Status fire_timers(core::UnixNanos limit, bool inclusive, bool* fired = nullptr) {
    core::FiredTimer due;
    while (!halted() && engine_->next_timer(due) &&
           (due.deadline < limit || (inclusive && due.deadline == limit))) {
      if (fired != nullptr) {
        *fired = true;
      }
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
    const core::Status s = step_input(key, event);
    if (!core::ok(s) || !gating_ || gate_active_) {
      return s;
    }
    return gate(key.ts);
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
    if constexpr (VenueSource<Source>) {
      s = source_->on_outputs(key.ts, engine_->outputs());
      if (!core::ok(s)) {
        return s;
      }
    }
    summary_.outputs += engine_->outputs().size();
    s = engine_->flush_outputs(*recorder_);
    if (!core::ok(s)) {
      return s;
    }
    if constexpr (requires { recorder_->after_step(*engine_, key); }) {
      recorder_->after_step(*engine_, key); // telemetry: the step's time and log records
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
    if (const auto* admin = std::get_if<model::AdminCommand>(&event);
        admin != nullptr && admin->action == model::AdminAction::Snapshot) {
      snapshots_.request();
    }
    if (snapshots_.due(key.seq, std::holds_alternative<model::BatchEnd>(event))) {
      if constexpr (requires { recorder_->snapshot(*engine_, key); }) {
        ++(core::ok(recorder_->snapshot(*engine_, key)) ? summary_.snapshots
                                                        : summary_.snapshot_failures);
      }
    }
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
  engine::SnapshotSchedule snapshots_;
  RunSummary summary_;
  std::uint64_t seq_ = 0;
  bool gating_ = false;      // after start: every input may move the lifecycle
  bool gate_active_ = false; // the gate's own transitions do not re-enter it
  bool draining_ = false;    // shutting down: inputs are stepped even after a halt
  bool batch_open_ = false;
  core::UnixNanos batch_ts_;
  core::UnixNanos last_ts_;
};

} // namespace jarvis::backtest
