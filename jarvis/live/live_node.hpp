#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/startup.hpp"
#include "jarvis/backtest/driver.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/live/admin_pump.hpp"
#include "jarvis/live/admin_server.hpp"
#include "jarvis/live/clock.hpp"
#include "jarvis/live/live_source.hpp"
#include "jarvis/live/market_feed.hpp"
#include "jarvis/live/persist.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/sandbox_node.hpp"
#include "jarvis/live/spsc_ring.hpp"
#include "jarvis/live/venue_io.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/node/admin_protocol.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/credentials.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// The live node (docs/architecture.md sections 4.1, 7.1 and 15): market data from the md-io
// thread (MarketFeed), the account and order entry from the venue-io thread (VenueIo), and the
// calling thread as the core thread, running the same driver and engine as a backtest in real
// time. Before the loop:
//
//   1. the credentials (venues[0].credentials) and the startup checks (clock, instruments,
//      position mode, margin type, leverage, key permissions): a failed check stops the node;
//   2. the next ClientOrderId epoch, persisted before any order can be sent;
//   3. the instruments' definitions as the preamble.
//
// The node then waits in Syncing until the account is reconciled (the driver's sync gate), and
// moves between Running and Degraded with the user data stream. Every input is recorded (by the
// persist thread), so the session replays under the backtest wiring. Commands go to the venue-io
// thread as the engine emits them, after their own records (with persistence.mode = "barrier"
// the venue-io thread sends each once its record is durable); on the way out the node waits (a
// bounded time) until they have left the ring.

namespace jarvis::live {

struct LiveRequest {
  const node::NodeConfig* config = nullptr;
  node::RunManifest manifest;
  std::string out; // run directory; empty for persistence.dir
  node::HeaderExtras extras;
  std::optional<FeedEndpoints> market;  // default: from venues[0].endpoint
  std::optional<VenueEndpoints> venue;  // default: from venues[0].endpoint
  std::optional<std::string> spot_rest; // key permissions; default api.binance.com (none on
                                        // testnet, which has no such endpoint)
  std::optional<node::ApiCredentials> credentials; // default: venues[0].credentials
  std::string epoch_file; // default: "epoch" beside the run directories of persistence.dir
  bool allow_unrestricted_ip = false;
  const std::atomic<bool>* stop = nullptr;
  std::optional<std::chrono::nanoseconds> run_for;
  std::function<std::int64_t()> now_ms; // local UTC milliseconds for signing (tests)
};

struct LiveResult {
  std::string directory; // empty when persistence.mode = "none"
  backtest::RunSummary summary;
  MarketFeedStats feed;
  VenueIoStats venue;
  adapter::binance::StartupReport startup;
  std::uint64_t epoch = 0;
  std::uint64_t admin_commands = 0; // taken from the admin socket
  PersistStats persist;
};

// What is built before the loop starts.
struct LivePlan {
  std::vector<adapter::binance::PerpetualDefinition> instruments;
  node::Preamble preamble;
  MarketFeedConfig feed;
  VenueIoConfig venue; // identity strategies are filled from the engine
  adapter::binance::StartupReport startup;
  std::uint64_t epoch = 0;
};

[[nodiscard]] core::Status plan_live(const LiveRequest& request, core::UnixNanos now, LivePlan& out,
                                     std::string& error);

// "epoch" in the directory persistence.dir puts the runs of this node in.
[[nodiscard]] std::string default_epoch_file(const node::NodeConfig& config);

// The driver's pump in real time: the admin socket's commands, then the account ring (its answers
// come before market data, section 5.5), then the market data ring, all into the one live
// source.
template <typename Hook> class LivePump {
public:
  LivePump(const MonotonicClock& clock, MarketFeed& feed, VenueIo& venue, LiveSource& source,
           Hook& hook, const std::atomic<bool>* stop, std::optional<core::UnixNanos> deadline)
      : clock_{&clock}, feed_{&feed}, venue_{&venue}, source_{&source}, hook_{&hook}, stop_{stop},
        deadline_{deadline} {}

  [[nodiscard]] core::UnixNanos now() const { return clock_->now(); }

  [[nodiscard]] core::Status pump(core::UnixNanos now) {
    core::Status s = pump_admin(admin_, kernel_, *source_, now);
    if (core::ok(s)) {
      s = drain(venue_->ring(), venue_->source_id(), now);
    }
    return core::ok(s) ? drain(feed_->ring(), feed_->source_id(), now) : s;
  }

  // The admin socket's commands come first in every round; `kernel` is published for status.
  void attach_admin(AdminServer* admin, const strategy::KernelServices* kernel) noexcept {
    admin_ = admin;
    kernel_ = kernel;
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
  static constexpr std::size_t kMaxPerPump = 4096;

  core::Status drain(SpscByteRing& ring, std::uint16_t source_id, core::UnixNanos now) {
    for (std::size_t n = 0; n < kMaxPerPump; ++n) {
      bool empty = false;
      const std::span<const std::byte> record = ring.peek(empty);
      if (empty) {
        break;
      }
      const core::Status s = source_->push_record(record, now, source_id);
      ring.release();
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  const MonotonicClock* clock_;
  MarketFeed* feed_;
  VenueIo* venue_;
  LiveSource* source_;
  Hook* hook_;
  const std::atomic<bool>* stop_;
  std::optional<core::UnixNanos> deadline_;
  AdminServer* admin_ = nullptr;
  const strategy::KernelServices* kernel_ = nullptr;
};

// A recorder that also hands the venue commands to the venue-io thread. A full command ring is
// waited out (a command is never dropped) unless the node is stopping. With a barrier (the
// persist thread in barrier mode) each command carries the log position after its own record.
template <typename Rec> class CommandRouter {
public:
  CommandRouter(Rec& inner, SpscRing<QueuedCommand>& commands, const std::atomic<bool>* stop,
                const Persister* barrier = nullptr)
      : inner_{&inner}, commands_{&commands}, stop_{stop}, barrier_{barrier} {}

  [[nodiscard]] core::Status record(const core::EventKey& key, const model::Event& event) {
    return inner_->record(key, event);
  }

  [[nodiscard]] core::Status emit(const core::EventKey& key, const model::Output& output) {
    const core::Status s = inner_->emit(key, output);
    if (!core::ok(s)) {
      return s;
    }
    if (const auto* c = std::get_if<model::SubmitOrder>(&output)) {
      return send(VenueCommand{*c});
    }
    if (const auto* c = std::get_if<model::ModifyOrder>(&output)) {
      return send(VenueCommand{*c});
    }
    if (const auto* c = std::get_if<model::CancelOrder>(&output)) {
      return send(VenueCommand{*c});
    }
    if (const auto* c = std::get_if<model::CountdownCancelAll>(&output)) {
      return send(VenueCommand{*c});
    }
    return core::Status::Ok;
  }

private:
  core::Status send(const VenueCommand& c) {
    const QueuedCommand q{c, barrier_ != nullptr ? barrier_->position() : 0};
    while (!commands_->try_push(q)) {
      if (stop_ != nullptr && stop_->load(std::memory_order_relaxed)) {
        return core::Status::IoError;
      }
      std::this_thread::yield();
    }
    return core::Status::Ok;
  }

  Rec* inner_;
  SpscRing<QueuedCommand>* commands_;
  const std::atomic<bool>* stop_;
  const Persister* barrier_;
};

namespace detail {

// Waits up to `limit` for the venue-io thread to take the commands still in the ring. With a
// barrier the persist thread must have been closed first, so that every record is durable.
void await_commands(VenueIo& venue, std::chrono::milliseconds limit);

template <strategy::StrategySet SS, typename Recorder, typename Hook>
[[nodiscard]] core::Status live_loop(const node::NodeConfig& config, engine::Engine<SS>& engine,
                                     const LivePlan& plan, LiveSource& source, Recorder& recorder,
                                     VenueIo& venue, LivePump<Hook>& pump,
                                     const std::atomic<bool>* stop, const Persister* barrier,
                                     LiveResult& result, std::string& error) {
  CommandRouter<Recorder> router{recorder, venue.commands(), stop, barrier};
  backtest::DriverOptions options;
  options.preamble = plan.preamble.events;
  options.await_sync = true;
  node::shutdown_options(config, options);
  backtest::Driver driver{engine, source, router, options};
  const core::Status s = driver.run_realtime(pump, result.summary);
  if (!core::ok(s)) {
    error = "the run stopped at seq " + std::to_string(engine.kernel().current.seq) + ": " +
            std::string{core::to_string(s)};
  }
  return s;
}

} // namespace detail

template <strategy::StrategySet SS, node::InputHook Hook>
[[nodiscard]] core::Status run_live(const LiveRequest& request, SS& strategies, LiveResult& result,
                                    std::string& error, Hook& hook) {
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
  LivePlan plan;
  core::Status s = plan_live(request, start, plan, error);
  result.startup = plan.startup;
  result.epoch = plan.epoch;
  if (!core::ok(s)) {
    return s;
  }
  strategy::KernelConfig kernel = node::kernel_config(config);
  kernel.trading.epoch = plan.epoch;
  engine::Engine<SS> engine{kernel, strategies, node::error_policy(config)};
  node::name_strategies(config, engine.kernel());
  const core::FixedVector<model::StrategyId>& ids = engine.kernel().trading.strategy_ids;
  plan.venue.identity.strategies.assign(ids.span().begin(), ids.span().end());

  // Declared before the IO threads: venue-io reads its durable position until it stops.
  std::unique_ptr<Persister> persister;
  const bool persist = config.persistence.mode != node::PersistenceMode::None;
  const bool barrier = config.persistence.mode == node::PersistenceMode::Barrier;
  if (persist) {
    s = node::create_run_directory(config, request.manifest, request.out, result.directory, error);
    if (!core::ok(s)) {
      return s;
    }
    persister = std::make_unique<Persister>(persist_config(config));
    s = persister->open(result.directory, node::run_header(config, request.extras), error);
    if (!core::ok(s)) {
      return s;
    }
    if (config.persistence.raw_frames != node::RawFrames::Off) {
      plan.feed.raw_frames = result.directory + "/raw-frames.jraw";
      plan.venue.raw_frames = result.directory + "/raw-account.jraw";
    }
    if (barrier) {
      plan.venue.durable = &persister->durable();
    }
  }
  MarketFeed feed{anchor, plan.feed};
  VenueIo venue{anchor, plan.venue};
  std::unique_ptr<AdminServer> admin;
  if (const std::string path = node::admin_socket_path(config); !path.empty()) {
    admin = std::make_unique<AdminServer>(path);
    s = admin->start(error);
  }
  if (core::ok(s)) {
    s = feed.start(error);
  }
  if (core::ok(s)) {
    s = venue.start(error);
  }
  if (!core::ok(s)) {
    feed.stop();
    return s;
  }
  LiveSource source;
  std::optional<core::UnixNanos> deadline;
  if (request.run_for) {
    deadline =
        core::UnixNanos{start.value() + static_cast<std::uint64_t>(request.run_for->count())};
  }
  LivePump<Hook> pump{clock, feed, venue, source, hook, request.stop, deadline};
  pump.attach_admin(admin.get(), &engine.kernel());
  if (persist) {
    node::LogRecorder<Hook, Persister> recorder{*persister, hook};
    s = detail::live_loop(config, engine, plan, source, recorder, venue, pump, request.stop,
                          barrier ? persister.get() : nullptr, result, error);
  } else {
    node::NullRecorder<Hook> recorder{hook};
    s = detail::live_loop(config, engine, plan, source, recorder, venue, pump, request.stop,
                          nullptr, result, error);
  }
  // Nothing is appended once the loop has ended: the log is closed (every record durable) before
  // the last commands, which in barrier mode wait for that, are let out.
  core::Status closed = core::Status::Ok;
  if (persist) {
    closed = persister->close();
    result.persist = persister->stats();
  }
  detail::await_commands(venue, std::chrono::milliseconds{2'000});
  venue.stop();
  feed.stop();
  if (admin) {
    admin->stop();
    result.admin_commands = admin->accepted();
  }
  result.feed = feed.stats();
  result.venue = venue.stats();
  if (core::ok(s) && !core::ok(closed)) {
    error = "cannot finish the run log in " + result.directory + ": " +
            std::string{core::to_string(closed)};
    return closed;
  }
  return s;
}

} // namespace jarvis::live
