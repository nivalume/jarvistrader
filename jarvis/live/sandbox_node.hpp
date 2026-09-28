#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/backtest/driver.hpp"
#include "jarvis/backtest/venue_loop.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/live/admin_pump.hpp"
#include "jarvis/live/admin_server.hpp"
#include "jarvis/live/clock.hpp"
#include "jarvis/live/live_source.hpp"
#include "jarvis/live/market_feed.hpp"
#include "jarvis/node/admin_protocol.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// The sandbox node (docs/architecture.md sections 4.1 and 4.6): real market data from the
// venue, orders against the simulated exchange, in real time. The md-io thread (MarketFeed)
// decodes and syncs the books; the calling thread is the core thread and runs the same driver
// and engine as a backtest, in real time (Driver::run_realtime). The simulated venue sees each
// market data event when the node does; order latency comes from [venues.sim]. Every input is
// recorded, so `jarvis replay` re-runs a sandbox session under the backtest wiring and must
// produce the same outputs (the environment equivalence of section 4.6).

namespace jarvis::live {

struct SandboxRequest {
  const node::NodeConfig* config = nullptr;
  node::RunManifest manifest;
  std::string out; // run directory; empty for persistence.dir
  node::HeaderExtras extras;
  std::optional<FeedEndpoints> endpoints;  // default: from venues[0].endpoint
  std::string rest_base;                   // exchangeInfo; default: from venues[0].endpoint
  const std::atomic<bool>* stop = nullptr; // a shutdown request (signal, admin)
  std::optional<std::chrono::nanoseconds> run_for;
};

struct SandboxResult {
  std::string directory; // empty when persistence.mode = "none"
  backtest::RunSummary summary;
  MarketFeedStats feed;
  std::uint64_t admin_commands = 0; // taken from the admin socket
};

// What is built before the loop starts: the instruments (exchangeInfo), the preamble (their
// definitions and the simulated account, stamped `now`), and the feed configuration.
struct SandboxPlan {
  std::vector<adapter::binance::PerpetualDefinition> instruments;
  node::Preamble preamble;
  MarketFeedConfig feed;
};

[[nodiscard]] core::Status plan_sandbox(const SandboxRequest& request, core::UnixNanos now,
                                        SandboxPlan& out, std::string& error);

// Shared with the live node: the venue symbols [data.streams] names (USDⓈ-M perpetuals) and
// their full stream names; the instruments into the feed's symbol table and the preamble.
[[nodiscard]] core::Status feed_streams(const node::NodeConfig& config,
                                        std::vector<std::string>& symbols,
                                        std::vector<std::string>& streams, std::string& error);
[[nodiscard]] core::Status
feed_instruments(std::span<const adapter::binance::PerpetualDefinition> instruments,
                 MarketFeedConfig& feed, node::Preamble& preamble, std::string& error);

// While alive, SIGINT and SIGTERM set `flag` (the handlers only store to the atomic); the
// previous handlers (Python's, in a Python process) come back when it goes.
class ShutdownSignals {
public:
  explicit ShutdownSignals(std::atomic<bool>& flag);
  ~ShutdownSignals();
  ShutdownSignals(const ShutdownSignals&) = delete;
  ShutdownSignals& operator=(const ShutdownSignals&) = delete;

private:
  struct Saved;
  std::unique_ptr<Saved> saved_;
};

// The driver's pump in real time: the node clock, the feed ring drained into the live source,
// the idle hook, the stop request.
template <typename Hook> class SandboxPump {
public:
  SandboxPump(const MonotonicClock& clock, MarketFeed& feed, LiveSource& source, Hook& hook,
              const std::atomic<bool>* stop, std::optional<core::UnixNanos> deadline)
      : clock_{&clock}, feed_{&feed}, source_{&source}, hook_{&hook}, stop_{stop},
        deadline_{deadline} {}

  [[nodiscard]] core::UnixNanos now() const { return clock_->now(); }

  // The admin socket's commands come first in every round; `kernel` is published for status.
  void attach_admin(AdminServer* admin, const strategy::KernelServices* kernel) noexcept {
    admin_ = admin;
    kernel_ = kernel;
  }

  [[nodiscard]] core::Status pump(core::UnixNanos now) {
    if (const core::Status s = pump_admin(admin_, kernel_, *source_, now); !core::ok(s)) {
      return s;
    }
    SpscByteRing& ring = feed_->ring();
    for (std::size_t n = 0; n < kMaxPerPump; ++n) {
      bool empty = false;
      const std::span<const std::byte> record = ring.peek(empty);
      if (empty) {
        break;
      }
      const core::Status s = source_->push_record(record, now, feed_->source_id());
      ring.release();
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  [[nodiscard]] core::Status idle(core::UnixNanos now) {
    if constexpr (requires { hook_->on_idle(now); }) {
      hook_->on_idle(now);
    }
    std::this_thread::yield();
    return core::Status::Ok;
  }

  [[nodiscard]] bool stop_requested() const {
    return (stop_ != nullptr && stop_->load(std::memory_order_relaxed)) ||
           (deadline_ && !(clock_->now() < *deadline_));
  }

private:
  static constexpr std::size_t kMaxPerPump = 4096; // bounds one pass; the rest waits a turn

  const MonotonicClock* clock_;
  MarketFeed* feed_;
  LiveSource* source_;
  Hook* hook_;
  const std::atomic<bool>* stop_;
  std::optional<core::UnixNanos> deadline_;
  AdminServer* admin_ = nullptr;
  const strategy::KernelServices* kernel_ = nullptr;
};

namespace detail {

template <strategy::StrategySet SS, typename Source, typename Recorder, typename Hook>
[[nodiscard]] core::Status run_loop(engine::Engine<SS>& engine, Source& source, Recorder& recorder,
                                    const backtest::DriverOptions& options, SandboxPump<Hook>& pump,
                                    SandboxResult& result, std::string& error) {
  backtest::Driver driver{engine, source, recorder, options};
  const core::Status s = driver.run_realtime(pump, result.summary);
  if (!core::ok(s)) {
    error = "the run stopped at seq " + std::to_string(engine.kernel().current.seq) + ": " +
            std::string{core::to_string(s)};
  }
  return s;
}

template <strategy::StrategySet SS, typename Recorder, typename Hook>
[[nodiscard]] core::Status
run_wired(const node::NodeConfig& config, engine::Engine<SS>& engine, const SandboxPlan& plan,
          LiveSource& source, Recorder& recorder, SandboxPump<Hook>& pump, core::UnixNanos now,
          SandboxResult& result, std::string& error) {
  backtest::DriverOptions options;
  options.preamble = plan.preamble.events;
  node::shutdown_options(config, options);
  if (config.venues.empty() || !config.venues.front().sim) {
    return run_loop(engine, source, recorder, options, pump, result, error);
  }
  backtest::VenueLoopConfig venue;
  core::Status s = node::venue_loop_config(config, engine.kernel().config(), venue, error);
  if (!core::ok(s)) {
    return s;
  }
  venue.live_feed = true;
  venue.start.reset();
  venue.end.reset();
  auto loop = std::make_unique<backtest::VenueLoop<LiveSource>>(venue, source);
  const core::FixedVector<model::StrategyId>& ids = engine.kernel().trading.strategy_ids;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    loop->exchange().set_strategy_id(static_cast<std::uint16_t>(i), ids[i]);
  }
  for (const model::Event& e : plan.preamble.events) {
    s = loop->exchange().on_data(e, now);
    if (!core::ok(s)) {
      error = "the simulated venue refused the preamble: " + std::string{core::to_string(s)};
      return s;
    }
  }
  return run_loop(engine, *loop, recorder, options, pump, result, error);
}

} // namespace detail

template <strategy::StrategySet SS, node::InputHook Hook>
[[nodiscard]] core::Status run_sandbox(const SandboxRequest& request, SS& strategies,
                                       SandboxResult& result, std::string& error, Hook& hook) {
  const node::NodeConfig& config = *request.config;
  if (strategies.size() > config.node.capacity.strategies) {
    error = "the node has " + std::to_string(strategies.size()) +
            " strategies but node.capacity.strategies is " +
            std::to_string(config.node.capacity.strategies);
    return core::Status::CapacityExceeded;
  }
  const ArrivalClock anchor;
  const MonotonicClock clock{anchor};
  const core::UnixNanos start = clock.now();
  SandboxPlan plan;
  core::Status s = plan_sandbox(request, start, plan, error);
  if (!core::ok(s)) {
    return s;
  }
  engine::Engine<SS> engine{node::kernel_config(config), strategies, node::error_policy(config)};
  node::name_strategies(config, engine.kernel());

  node::EventLogWriter writer;
  const bool persist = config.persistence.mode != node::PersistenceMode::None;
  if (persist) {
    s = node::create_run_directory(config, request.manifest, request.out, result.directory, error);
    if (!core::ok(s)) {
      return s;
    }
    node::EventLogOptions log_options;
    log_options.sync_on_flush = config.persistence.mode == node::PersistenceMode::Barrier;
    s = writer.open(result.directory, node::run_header(config, request.extras), log_options);
    if (!core::ok(s)) {
      error = "cannot open the run log in " + result.directory;
      return s;
    }
    if (config.persistence.raw_frames != node::RawFrames::Off) {
      plan.feed.raw_frames = result.directory + "/raw-frames.jraw";
    }
  }
  MarketFeed feed{anchor, plan.feed};
  std::unique_ptr<AdminServer> admin;
  if (const std::string path = node::admin_socket_path(config); !path.empty()) {
    admin = std::make_unique<AdminServer>(path);
    s = admin->start(error);
  }
  if (core::ok(s)) {
    s = feed.start(error);
  }
  if (!core::ok(s)) {
    return s;
  }
  LiveSource source;
  std::optional<core::UnixNanos> deadline;
  if (request.run_for) {
    deadline =
        core::UnixNanos{start.value() + static_cast<std::uint64_t>(request.run_for->count())};
  }
  SandboxPump<Hook> pump{clock, feed, source, hook, request.stop, deadline};
  pump.attach_admin(admin.get(), &engine.kernel());
  if (persist) {
    node::LogRecorder<Hook> recorder{writer, hook};
    s = detail::run_wired(config, engine, plan, source, recorder, pump, start, result, error);
  } else {
    node::NullRecorder<Hook> recorder{hook};
    s = detail::run_wired(config, engine, plan, source, recorder, pump, start, result, error);
  }
  feed.stop();
  if (admin) {
    admin->stop();
    result.admin_commands = admin->accepted();
  }
  result.feed = feed.stats();
  if (persist) {
    const core::Status closed = writer.close();
    if (core::ok(s) && !core::ok(closed)) {
      error = "cannot finish the run log in " + result.directory;
      return closed;
    }
  }
  return s;
}

} // namespace jarvis::live
