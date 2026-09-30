#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/live/clock.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/telemetry.hpp"

// Telemetry of a sandbox or live node (docs/architecture.md section 19.2).
//
//   JSON lines   what the node did, one object per line in `telemetry.jsonl` beside the run log:
//                the lifecycle, connections, reconciliation, admin commands, strategy errors,
//                every order command and order event (with its client_order_id as the trace
//                id), and the kernel's log records (TradingState changes, halted strategies,
//                kill switches), each with the seq and ts of the input it came from;
//   Prometheus   the metrics of section 19.2 as text at http://<telemetry.prometheus>/metrics,
//                with /ready (200 when Running, else 503) and /live (200 while the core
//                thread publishes, else 503).
//
// The core thread only copies fixed-size records into a ring and, every 100 ms, a sample of the
// kernel's and the IO threads' counters into a buffer; the telemetry thread formats, counts,
// writes the file and serves HTTP. Nothing here reaches the kernel's state: the samples read
// it through const accessors, between steps.

namespace jarvis::live {

// ---- latency histograms (integer nanosecond buckets) ----------------------------------------

inline constexpr std::array<std::uint64_t, 17> kLatencyBoundsNs = {
    250,     500,     1'000,     2'000,     5'000,     10'000,     20'000,     50'000,     100'000,
    200'000, 500'000, 1'000'000, 2'000'000, 5'000'000, 10'000'000, 50'000'000, 100'000'000};
inline constexpr std::size_t kLatencyBuckets = kLatencyBoundsNs.size() + 1; // the last: +Inf

struct HistogramData {
  std::array<std::uint64_t, kLatencyBuckets> buckets{}; // per bucket, not cumulative
  std::uint64_t count = 0;
  std::uint64_t sum_ns = 0;

  void add(std::uint64_t ns) noexcept;
  // The upper bound of the bucket holding quantile `q` (0 < q <= 1); UINT64_MAX in +Inf, 0 when
  // empty.
  [[nodiscard]] std::uint64_t quantile_ns(double q) const noexcept;
};

// Written by one thread, read by others.
class AtomicHistogram {
public:
  void add(std::uint64_t ns) noexcept;
  [[nodiscard]] HistogramData read() const noexcept;

private:
  std::array<std::atomic<std::uint64_t>, kLatencyBuckets> buckets_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
};

// ---- records ------------------------------------------------------------------------------

// A VenueSnapshot without its variable-length parts.
struct SnapshotSummary {
  model::AccountId account_id;
  bool check = false; // a light check
  std::uint32_t orders = 0;
  std::uint32_t fills = 0;
  std::uint32_t positions = 0;
  std::uint32_t balances = 0;
  core::UnixNanos ts_snapshot;
};

using TelemetryPayload =
    std::variant<strategy::LogRecord, SnapshotSummary, model::NodeLifecycle,
                 model::ConnectionStatus, model::StrategyError, model::AdminCommand,
                 model::Shutdown, model::RunStart, model::RateLimitFeedback, model::OrderAccepted,
                 model::OrderRejected, model::OrderCanceled, model::OrderExpired,
                 model::OrderTriggered, model::OrderModifyRejected, model::OrderCancelRejected,
                 model::OrderUpdated, model::OrderFilled, model::OrderFillVoided,
                 model::SubmitOrder, model::ModifyOrder, model::CancelOrder, model::CancelAllOrders,
                 model::OrderDenied, model::ReconciliationDiff, model::ReconcileOutcome,
                 model::CountdownCancelAll>;

struct TelemetryRecord {
  std::uint64_t ts = 0;  // the input's ts
  std::uint64_t seq = 0; // the input's seq
  TelemetryPayload payload;
};

// The record of an input or an output, when the telemetry logs it (market data, timers, batch
// ends, instrument definitions and features are counted, not logged).
[[nodiscard]] bool telemetry_record(const core::EventKey& key, const model::Event& event,
                                    TelemetryRecord& out);
[[nodiscard]] bool telemetry_record(const core::EventKey& key, const model::Output& output,
                                    TelemetryRecord& out);
[[nodiscard]] TelemetryRecord telemetry_record(const core::EventKey& key,
                                               const strategy::LogRecord& log);

// Market data: its arrival makes the market data age 0.
[[nodiscard]] bool is_market_data(const model::Event& event) noexcept;

// One JSON object, without the newline: ts, seq, event, then the payload's fields (decimals and
// identifiers as strings, times as integer nanoseconds, enums by name, absent values as null).
[[nodiscard]] std::string json_line(const TelemetryRecord& record);

// ---- metrics ------------------------------------------------------------------------------

struct RingSample {
  std::string name;
  std::uint64_t used = 0; // bytes or slots
  std::uint64_t capacity = 0;
  std::uint64_t high_water = 0; // the most seen in a sample
};

struct RateWindowSample {
  std::uint64_t interval_ns = 0;
  std::uint32_t used = 0;
  std::uint32_t limit = 0;
};

struct ExposureSample {
  std::string instrument;
  double notional = 0;  // the larger side, open orders included, at the mark
  std::string currency; // of the notional
  double ratio = -1;    // notional / risk.max_position_notional; -1 without a limit
};

struct StrategySample {
  std::string id;
  std::uint64_t calls = 0;
  std::uint64_t total_ns = 0;
  std::uint64_t max_ns = 0;
  std::uint64_t overruns = 0;
};

// What the core thread publishes every 100 ms.
struct MetricsSample {
  std::uint64_t published_ns = 0; // network::steady_ns() at publication
  model::NodeState node_state = model::NodeState::Init;
  model::TradingState trading_state = model::TradingState::Active;
  std::uint64_t seq = 0;
  std::uint64_t inputs = 0;
  std::uint64_t outputs = 0;
  std::vector<RingSample> rings;
  HistogramData step;            // record to the end of the step, outputs handed on
  HistogramData tick_to_command; // market data arrival to the command in the venue-io ring
  HistogramData command_to_socket;
  bool market_data_seen = false;
  std::uint64_t market_data_age_ns = 0;
  std::map<std::string, std::uint64_t> counters; // name -> value (monotonic)
  std::map<std::string, std::uint64_t> gauges;   // name -> value
  std::vector<RateWindowSample> rate_windows;
  std::vector<ExposureSample> exposures;
  std::vector<StrategySample> strategies;
};

// What the telemetry thread counts from the records.
struct RecordCounts {
  std::uint64_t records = 0;
  std::map<std::string, std::uint64_t> events;   // event name -> count
  std::map<std::string, std::uint64_t> denied;   // reason -> count
  std::map<std::string, std::uint64_t> rejected; // venue reason code -> count
  std::map<std::string, std::uint64_t> diffs;    // ReconcileDiffKind -> count
  std::map<std::string, std::uint64_t> connection_downs;
  std::map<std::string, bool> connection_up;
  std::map<std::string, double> fees_actual;          // currency -> sum
  std::map<std::string, double> fees_estimated;       // currency -> sum at the configured rates
  std::map<std::string, RateWindowSample> venue_rate; // "<kind>_<interval>" -> last feedback

  void count(const TelemetryRecord& record, double maker_rate, double taker_rate);
};

// The Prometheus text exposition of one sample and the counts; `alive` as the scrape sees it.
[[nodiscard]] std::string prometheus_text(const MetricsSample& sample, const RecordCounts& counts,
                                          bool alive, std::uint64_t dropped);

// ---- the telemetry thread -----------------------------------------------------------------

struct TelemetryConfig {
  std::string listen;     // "host:port" for Prometheus; empty: no HTTP server
  std::string jsonl_path; // empty: no JSON lines
  std::size_t ring_slots = 8192;
  std::chrono::milliseconds publish_every{100};
  std::chrono::milliseconds alive_within{5'000};
  double maker_rate = 0.0002; // for the fee estimate (binance_usdm_vip0 unless [venues.sim])
  double taker_rate = 0.0005;
  std::vector<int> cpus; // the telemetry thread's CPUs (cpu_affinity.hpp); empty: any
};

class MarketFeed;
class VenueIo;
class Persister;

// What the core thread samples besides its own counters; null parts are left out.
struct TelemetrySources {
  const strategy::KernelServices* kernel = nullptr;
  MarketFeed* feed = nullptr;
  VenueIo* venue = nullptr;
  const Persister* persister = nullptr;
  std::function<void(std::vector<StrategySample>&)> strategies; // Python hosts' timings
};

class Telemetry {
public:
  explicit Telemetry(TelemetryConfig config);
  ~Telemetry();
  Telemetry(const Telemetry&) = delete;
  Telemetry& operator=(const Telemetry&) = delete;

  // Opens the file, binds the listener and starts the thread.
  [[nodiscard]] core::Status start(std::string& error);
  void stop();                                       // drains the ring, writes the file out, joins
  [[nodiscard]] std::uint16_t port() const noexcept; // the bound port (0 without a listener)

  // ---- the core thread ----
  // An input about to be stepped, and the outputs and log records of its step (TelemetryRecorder
  // calls these); `now` is the node clock, for the tick-to-command latency.
  void on_input(const core::EventKey& key, const model::Event& event);
  void on_output(const core::EventKey& key, const model::Output& output, core::UnixNanos now);
  void on_step_end(const core::EventKey& key, std::span<const strategy::LogRecord> logs);
  // Every round of the real-time loop: a sample every publish_every (a cheap check otherwise);
  // `force` samples now (the last one, as the node stops).
  void sample(const TelemetrySources& sources, core::UnixNanos now, bool force = false);

  void push(const TelemetryRecord& record) noexcept; // a full ring drops (counted)
  [[nodiscard]] bool publish_due(std::uint64_t steady_ns) const noexcept;
  [[nodiscard]] MetricsSample& sample_buffer() noexcept; // fill, then publish()
  void publish(std::uint64_t steady_ns);

  // ---- any thread ----
  [[nodiscard]] std::uint64_t dropped() const noexcept;
  [[nodiscard]] std::uint64_t written() const noexcept; // JSON lines written
  [[nodiscard]] RecordCounts counts() const;            // a copy
  [[nodiscard]] std::string metrics_text() const;       // what /metrics serves now

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The steady clock the samples and latencies use, in nanoseconds.
[[nodiscard]] std::uint64_t steady_now_ns() noexcept;

// "host:port" -> the two parts; false when malformed.
[[nodiscard]] bool split_listen(std::string_view listen, std::string& host, std::string& port);

// The driver's recorder for a node with telemetry: forwards everything to `inner` and tells the
// telemetry about each input, output and step (a null telemetry only forwards).
template <typename Rec> class TelemetryRecorder {
public:
  TelemetryRecorder(Rec& inner, Telemetry* telemetry, const MonotonicClock& clock) noexcept
      : inner_{&inner}, telemetry_{telemetry}, clock_{&clock} {}

  [[nodiscard]] core::Status record(const core::EventKey& key, const model::Event& event) {
    if (telemetry_ != nullptr) {
      telemetry_->on_input(key, event);
    }
    return inner_->record(key, event);
  }

  [[nodiscard]] core::Status emit(const core::EventKey& key, const model::Output& output) {
    const core::Status s = inner_->emit(key, output);
    if (telemetry_ != nullptr && core::ok(s)) {
      telemetry_->on_output(key, output, clock_->now());
    }
    return s;
  }

  template <typename Engine>
    requires requires(Rec& r, Engine& e, const core::EventKey& k) { r.snapshot(e, k); }
  [[nodiscard]] core::Status snapshot(Engine& engine, const core::EventKey& key) {
    return inner_->snapshot(engine, key);
  }

  template <typename Engine> void after_step(Engine& engine, const core::EventKey& key) {
    if (telemetry_ != nullptr) {
      telemetry_->on_step_end(key, engine.logs());
    }
  }

private:
  Rec* inner_;
  Telemetry* telemetry_;
  const MonotonicClock* clock_;
};

} // namespace jarvis::live
