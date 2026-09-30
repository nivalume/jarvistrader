#include "jarvis/live/venue_io.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/adapter/binance/snapshot.hpp"
#include "jarvis/adapter/binance/user_stream.hpp"
#include "jarvis/adapter/binance/ws_api.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/network/timer.hpp"

namespace jarvis::live {

namespace {

using core::Status;
namespace binance = adapter::binance;

constexpr std::uint32_t kStreamConn = 0; // raw frame connection ids
constexpr std::uint32_t kApiConn = 1;

struct Counters {
  std::atomic<std::uint64_t> events{0};
  std::atomic<std::uint64_t> ring_waits{0};
  std::atomic<std::uint64_t> commands{0};
  std::atomic<std::uint64_t> refused_locally{0};
  std::atomic<std::uint64_t> unknown_outcomes{0};
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> decode_errors{0};
  std::atomic<std::uint64_t> snapshots{0};
  std::atomic<std::uint64_t> snapshot_failures{0};
  std::atomic<std::uint64_t> stale_snapshots{0};
  std::atomic<std::uint64_t> key_failures{0};
  std::atomic<std::uint64_t> countdowns{0};
  std::atomic<std::uint64_t> countdown_failures{0};
  std::atomic<std::uint64_t> checks{0};
  std::atomic<std::uint64_t> check_failures{0};
  std::atomic<std::uint64_t> rest_orders{0};
  std::atomic<std::uint64_t> barrier_waits{0};
  std::atomic<std::uint64_t> unsent_at_stop{0};

  static void add(std::atomic<std::uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }
};

std::span<const std::byte> bytes_of(std::string_view s) {
  return std::as_bytes(std::span<const char>{s.data(), s.size()});
}

std::string_view text_of(std::span<const std::byte> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()}; // NOLINT
}

// The blocking side: one RestClient, the listenKey, and snapshot jobs, on their own thread.
class RestThread {
public:
  using Job = std::function<void(binance::RestClient&)>;

  RestThread(binance::RestConfig config, std::chrono::milliseconds tick)
      : rest_{std::move(config)}, tick_{tick} {}

  void start(std::function<void(binance::RestClient&)> on_tick) {
    on_tick_ = std::move(on_tick);
    thread_ = std::thread{[this] { run(); }};
  }
  void stop() {
    {
      const std::lock_guard lock{mutex_};
      stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  // A job posted with `keep` still runs when the thread is stopped before it; others are dropped.
  void post(Job job, bool keep = false) {
    {
      const std::lock_guard lock{mutex_};
      jobs_.push_back(Entry{std::move(job), keep});
    }
    wake_.notify_all();
  }
  binance::RestClient& client() noexcept { return rest_; } // before start only

private:
  struct Entry {
    Job job;
    bool keep = false;
  };

  void run() {
    std::unique_lock lock{mutex_};
    while (!stopping_) {
      wake_.wait_for(lock, tick_, [this] { return stopping_ || !jobs_.empty(); });
      while (!stopping_ && !jobs_.empty()) {
        run_front(lock);
      }
      if (!stopping_) {
        lock.unlock();
        on_tick_(rest_);
        lock.lock();
      }
    }
    while (!jobs_.empty()) {
      if (jobs_.front().keep) {
        run_front(lock);
      } else {
        jobs_.pop_front();
      }
    }
  }

  void run_front(std::unique_lock<std::mutex>& lock) {
    Job job = std::move(jobs_.front().job);
    jobs_.pop_front();
    lock.unlock();
    job(rest_);
    lock.lock();
  }

  binance::RestClient rest_;
  std::chrono::milliseconds tick_;
  std::function<void(binance::RestClient&)> on_tick_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Entry> jobs_;
  bool stopping_ = false;
  std::thread thread_;
};

binance::RestConfig rest_config(const VenueIoConfig& c) {
  binance::RestConfig r;
  r.base_url = c.endpoints.rest;
  r.api_key = c.api_key;
  r.signer = c.signer;
  r.tls = c.endpoints.tls;
  r.now_ms = c.now_ms;
  return r;
}

} // namespace

struct VenueIo::Impl final : adapter::EventEmitter {
  Impl(const ArrivalClock& clock_, VenueIoConfig config_)
      : clock{&clock_}, config{std::move(config_)}, ring{config.ring_bytes},
        commands{config.command_slots}, tracker{config.symbols, config.identity},
        rest{rest_config(config), config.rest_tick},
        keeper{binance::rest_listen_keys(rest.client()), config.keeper},
        scratch(model::wire::kRecordHeaderSize + model::wire::kMaxPayload +
                model::wire::kRecordTrailerSize) {
    rest.client().set_time_offset(config.time_offset_ms);
    for (const binance::RecoveredOrder& r : config.recovered) {
      tracker.restore(r);
    }
  }

  const ArrivalClock* clock;
  VenueIoConfig config;
  SpscByteRing ring;
  SpscRing<QueuedCommand> commands;
  Counters counters;
  network::IoContext io;
  binance::OrderTracker tracker;
  AtomicHistogram command_latency;
  RestThread rest;
  binance::ListenKeyKeeper keeper; // the REST thread's
  bool key_started = false;        // the REST thread's
  std::int64_t next_key_try_ns = 0;
  std::vector<std::byte> scratch;
  std::unique_ptr<binance::WsApiSession> api;
  std::unique_ptr<binance::UserStreamSession> stream;
  std::unique_ptr<network::Timer> retry;
  std::unique_ptr<network::Timer> check_timer;
  bool head_held = false; // the command at the head of the ring waits for the persist thread
  RawFrameWriter raw;
  bool recording = false;
  bool live = false;            // the user stream (IO thread)
  std::uint64_t generation = 0; // changes with every live and down
  std::uint64_t synced = 0;     // the generation whose snapshot was recorded
  bool checking = false;        // a light check is being read
  std::uint64_t seed = 0;
  std::atomic<bool> stopping{false};
  std::thread thread;
  mutable std::mutex error_mutex;
  std::string error;

  void set_error(std::string e) {
    const std::lock_guard lock{error_mutex};
    error = std::move(e);
  }

  [[nodiscard]] core::UnixNanos utc(std::int64_t steady) const {
    return core::UnixNanos{clock->utc_of(steady)};
  }

  void record(RawKind kind, std::uint32_t conn, std::string_view text) {
    if (recording) {
      static_cast<void>(raw.write(RawFrame{clock->now(), conn, kind, 1, bytes_of(text)}));
    }
  }

  // EventEmitter: one kernel event into the ring, waiting while the ring is full.
  Status event(const model::Event& e) override {
    std::size_t written = 0;
    const Status s = model::wire::encode_record(
        core::EventKey{core::UnixNanos{clock->now()}, config.source_id, 0}, e, scratch, written);
    if (!core::ok(s)) {
      return s;
    }
    const std::span<const std::byte> record{scratch.data(), written};
    if (!ring.try_write(record)) {
      Counters::add(counters.ring_waits);
      while (!ring.try_write(record)) {
        if (stopping.load(std::memory_order_relaxed)) {
          return Status::IoError;
        }
        std::this_thread::yield();
      }
    }
    Counters::add(counters.events);
    return Status::Ok;
  }

  Status depth(const adapter::DepthDiff& /*d*/) override {
    return Status::UnsupportedMessage; // no market data here
  }

  void connection(model::ConnectionKind kind, bool up) {
    model::ConnectionStatus c;
    c.venue = config.venue;
    c.kind = kind;
    c.up = up;
    c.ts_init = core::UnixNanos{clock->now()};
    static_cast<void>(event(model::Event{c}));
  }

  // ---- the user stream ----------------------------------------------------------------------

  void on_live() {
    live = true;
    ++generation;
    record(RawKind::Open, kStreamConn, config.endpoints.user_stream);
    connection(model::ConnectionKind::UserStream, true);
    ask_snapshot(generation);
  }

  void on_down(const std::string& reason) {
    const bool was = live;
    live = false;
    ++generation;
    record(RawKind::Close, kStreamConn, reason);
    if (was) {
      connection(model::ConnectionKind::UserStream, false);
    }
  }

  void on_frame(std::span<const std::byte> frame, std::int64_t recv) {
    Counters::add(counters.frames);
    record(RawKind::Message, kStreamConn, text_of(frame));
    binance::UserReport report;
    std::string e;
    if (!core::ok(binance::decode_user_report(text_of(frame), report, e))) {
      Counters::add(counters.decode_errors);
      return;
    }
    if (std::holds_alternative<binance::ListenKeyExpiredReport>(report)) {
      rest.post([this](binance::RestClient& /*rest*/) { keeper.expired(); });
      return;
    }
    if (!core::ok(tracker.on_report(report, utc(recv), *this))) {
      Counters::add(counters.decode_errors);
    }
  }

  // ---- reconciliation snapshots -------------------------------------------------------------

  void ask_snapshot(std::uint64_t gen) {
    binance::AccountSnapshotRequest req;
    req.account_id = config.identity.account_id;
    for (std::uint32_t i = 0; i < config.symbols.size(); ++i) {
      req.symbols.push_back(config.symbols.venue_symbol(i));
    }
    req.orders = tracker.unclosed();
    req.next_trade = tracker.next_trades();
    req.trades_since_ms = config.trades_since_ms;
    req.seed = config.identity.seed ^ (++seed * 0x9E3779B97F4A7C15ULL);
    rest.post([this, gen, req = std::move(req)](binance::RestClient& client) {
      auto snap = std::make_shared<binance::AccountSnapshot>();
      std::string e;
      const Status s = binance::assemble_snapshot(client, config.symbols, req, *snap, e);
      for (const model::RateLimitFeedback& f : client.take_limits()) {
        io.post([this, f] { static_cast<void>(event(model::Event{f})); });
      }
      io.post([this, gen, s, snap, e = std::move(e)] { on_snapshot(gen, s, *snap, e); });
    });
  }

  void on_snapshot(std::uint64_t gen, Status s, const binance::AccountSnapshot& snap,
                   const std::string& e) {
    if (gen != generation || !live) {
      Counters::add(counters.stale_snapshots);
      return;
    }
    if (!core::ok(s)) {
      Counters::add(counters.snapshot_failures);
      set_error("reconciliation snapshot: " + e);
      retry->after(config.snapshot_retry, [this, gen] {
        if (gen == generation && live) {
          ask_snapshot(gen);
        }
      });
      return;
    }
    const model::VenueSnapshot v = snap.event(core::UnixNanos{clock->now()});
    if (!core::ok(event(model::Event{v}))) {
      Counters::add(counters.snapshot_failures);
      set_error("reconciliation snapshot: too large for a record");
      return;
    }
    Counters::add(counters.snapshots);
    tracker.absorb(v);
    synced = gen;
  }

  // ---- the light check (section 15.3) -------------------------------------------------------

  void schedule_check() {
    if (config.check_every.count() > 0) {
      check_timer->after(config.check_every, [this] {
        if (live && synced == generation && !checking) {
          ask_check(generation);
        }
        schedule_check();
      });
    }
  }

  void ask_check(std::uint64_t gen) {
    checking = true;
    binance::AccountSnapshotRequest req;
    req.account_id = config.identity.account_id;
    req.seed = config.identity.seed ^ (++seed * 0x9E3779B97F4A7C15ULL);
    rest.post([this, gen, req = std::move(req)](binance::RestClient& client) {
      auto snap = std::make_shared<binance::AccountSnapshot>();
      std::string e;
      const Status s = binance::assemble_check(client, config.symbols, req, *snap, e);
      for (const model::RateLimitFeedback& f : client.take_limits()) {
        io.post([this, f] { static_cast<void>(event(model::Event{f})); });
      }
      io.post([this, gen, s, snap, e = std::move(e)] { on_check(gen, s, *snap, e); });
    });
  }

  void on_check(std::uint64_t gen, Status s, const binance::AccountSnapshot& snap,
                const std::string& e) {
    checking = false;
    if (gen != generation || !live) {
      return; // the stream dropped meanwhile: a reconciliation follows
    }
    if (!core::ok(s)) {
      Counters::add(counters.check_failures);
      set_error("light check: " + e);
      return; // the next one comes at the next tick
    }
    if (!core::ok(event(model::Event{snap.event(core::UnixNanos{clock->now()})}))) {
      Counters::add(counters.check_failures);
      set_error("light check: too large for a record");
      return;
    }
    Counters::add(counters.checks);
  }

  // ---- the listenKey (REST thread) ----------------------------------------------------------

  void key_tick(binance::RestClient& /*client*/) {
    const std::int64_t now = network::steady_ns();
    if (!key_started) {
      if (now < next_key_try_ns) {
        return;
      }
      std::string e;
      if (!core::ok(keeper.start(now, e))) {
        Counters::add(counters.key_failures);
        set_error("listenKey: " + e);
        next_key_try_ns = now + std::chrono::nanoseconds{config.keeper.retry_after}.count();
        return;
      }
      key_started = true;
      io.post([this, key = keeper.key()] { stream->start(key); });
      return;
    }
    const std::uint64_t failures = keeper.stats().failures;
    if (keeper.tick(now)) {
      io.post([this, key = keeper.key()] { stream->set_listen_key(key); });
    }
    if (keeper.stats().failures != failures) {
      Counters::add(counters.key_failures);
      set_error("listenKey: " + keeper.last_error());
    }
  }

  // ---- order entry --------------------------------------------------------------------------

  template <typename C> void refuse(binance::RequestKind kind, const C& c, std::string_view why) {
    Counters::add(counters.refused_locally);
    binance::RequestError e;
    e.kind = kind;
    e.client_order_id.assign(c.client_order_id.view());
    e.message.assign(why);
    e.time_ms = clock->now() / 1'000'000;
    static_cast<void>(tracker.on_local_refusal(e, c.strategy_index, c.instrument_id,
                                               core::UnixNanos{clock->now()}, *this));
  }

  std::optional<std::string> symbol_of(const model::InstrumentId& id) const {
    const std::optional<std::uint32_t> i = config.symbols.find(id);
    return i ? std::optional{config.symbols.venue_symbol(*i)} : std::nullopt;
  }

  void command(const VenueCommand& c) {
    Counters::add(counters.commands);
    std::visit([this](const auto& cmd) { send(cmd); }, c);
  }

  // Each command goes on the WebSocket API when it is ready, else over REST (rest_fallback),
  // else it is refused here.
  void send(const model::SubmitOrder& c) {
    tracker.on_submit(c);
    const std::optional<std::string> symbol = symbol_of(c.instrument_id);
    if (!symbol) {
      refuse(binance::RequestKind::Place, c, "unknown instrument");
      return;
    }
    std::string e = "order entry is down";
    if (api->ready() && core::ok(api->place(c, *symbol, e))) {
      return;
    }
    binance::Params params;
    if (!config.rest_fallback || !core::ok(binance::place_params(c, *symbol, params, e))) {
      refuse(binance::RequestKind::Place, c, e);
      return;
    }
    over_rest(binance::RequestKind::Place, std::move(params));
  }
  void send(const model::ModifyOrder& c) {
    const std::optional<std::string> symbol = symbol_of(c.instrument_id);
    const std::optional<model::OrderSide> side = tracker.side_of(c.client_order_id.view());
    if (!symbol || !side) {
      refuse(binance::RequestKind::Modify, c, "unknown order");
      return;
    }
    std::string e = "order entry is down";
    if (api->ready() && core::ok(api->modify(c, *symbol, *side, e))) {
      return;
    }
    binance::Params params;
    if (!config.rest_fallback || !core::ok(binance::modify_params(c, *symbol, *side, params, e))) {
      refuse(binance::RequestKind::Modify, c, e);
      return;
    }
    over_rest(binance::RequestKind::Modify, std::move(params));
  }
  void send(const model::CancelOrder& c) {
    const std::optional<std::string> symbol = symbol_of(c.instrument_id);
    if (!symbol) {
      refuse(binance::RequestKind::Cancel, c, "unknown instrument");
      return;
    }
    std::string e = "order entry is down";
    if (api->ready() && core::ok(api->cancel(c, *symbol, e))) {
      return;
    }
    binance::Params params;
    if (!config.rest_fallback || !core::ok(binance::cancel_params(c, *symbol, params))) {
      refuse(binance::RequestKind::Cancel, c, e);
      return;
    }
    over_rest(binance::RequestKind::Cancel, std::move(params));
  }

  // The order fallback (section 14.4): the request goes on the REST thread, signed like any
  // other, and its outcome comes back here like the WebSocket API's. It still runs when the
  // venue-io stops (a shutdown's cancels).
  void over_rest(binance::RequestKind kind, binance::Params params) {
    Counters::add(counters.rest_orders);
    rest.post(
        [this, kind, params = std::move(params)](binance::RestClient& client) {
          binance::OrderOutcome o;
          o.request = kind;
          bool refused = false;
          Status s = Status::Ok;
          if (kind == binance::RequestKind::Place) {
            s = client.place(params, o.ack, o.error, refused, o.reason);
          } else if (kind == binance::RequestKind::Modify) {
            s = client.modify(params, o.ack, o.error, refused, o.reason);
          } else {
            s = client.cancel(params, o.ack, o.error, refused, o.reason);
          }
          o.recv_ns = network::steady_ns();
          if (!core::ok(s)) {
            o.kind = binance::OutcomeKind::Unknown;
          } else {
            o.kind = refused ? binance::OutcomeKind::Refused : binance::OutcomeKind::Acknowledged;
          }
          for (const model::RateLimitFeedback& f : client.take_limits()) {
            io.post([this, f] { static_cast<void>(event(model::Event{f})); });
          }
          io.post([this, o = std::move(o)] { on_outcome(o); });
        },
        /*keep=*/true);
  }

  // The dead man's switch goes over REST (the WebSocket API has no such method).
  void send(const model::CountdownCancelAll& c) {
    const std::optional<std::string> symbol = symbol_of(c.instrument_id);
    if (!symbol) {
      Counters::add(counters.countdown_failures);
      set_error("countdownCancelAll: unknown instrument " +
                std::string{c.instrument_id.text().view()});
      return;
    }
    rest.post(
        [this, symbol = *symbol, ms = c.countdown_ms](binance::RestClient& client) {
          std::string e;
          if (core::ok(client.countdown_cancel_all(symbol, ms, e))) {
            Counters::add(counters.countdowns);
          } else {
            Counters::add(counters.countdown_failures);
            set_error("countdownCancelAll " + symbol + ": " + e);
          }
        },
        /*keep=*/true);
  }

  void on_outcome(const binance::OrderOutcome& o) {
    const core::UnixNanos at = utc(o.recv_ns);
    switch (o.kind) {
    case binance::OutcomeKind::Acknowledged:
      if (o.request == binance::RequestKind::Place) {
        static_cast<void>(tracker.on_place_ack(o.ack, at, *this));
      } // a modify's or cancel's result arrives on the user stream
      break;
    case binance::OutcomeKind::Refused:
      static_cast<void>(tracker.on_request_error(o.error, at, *this));
      break;
    case binance::OutcomeKind::Unknown:
      Counters::add(counters.unknown_outcomes); // reconciliation settles it
      break;
    }
  }

  // ---- the thread ---------------------------------------------------------------------------

  void open() {
    binance::WsApiConfig ac;
    ac.url = config.endpoints.ws_api;
    ac.api_key = config.api_key;
    ac.signer = config.signer;
    ac.tls = config.endpoints.tls;
    ac.reconnect_initial = config.reconnect_initial;
    ac.reconnect_max = config.reconnect_max;
    ac.now_ms = config.now_ms;
    binance::WsApiHandlers ah;
    ah.on_ready = [this] {
      record(RawKind::Open, kApiConn, config.endpoints.ws_api);
      connection(model::ConnectionKind::OrderEntry, true);
    };
    ah.on_down = [this](const std::string& reason) {
      record(RawKind::Close, kApiConn, reason);
      connection(model::ConnectionKind::OrderEntry, false);
    };
    ah.on_outcome = [this](const binance::OrderOutcome& o) { on_outcome(o); };
    ah.on_limits = [this](std::span<const model::RateLimitFeedback> limits) {
      for (const model::RateLimitFeedback& f : limits) {
        static_cast<void>(event(model::Event{f}));
      }
    };
    api = std::make_unique<binance::WsApiSession>(io, std::move(ac), std::move(ah));
    api->set_time_offset(config.time_offset_ms);

    binance::UserStreamConfig uc;
    uc.url = config.endpoints.user_stream;
    uc.tls = config.endpoints.tls;
    uc.reconnect_initial = config.reconnect_initial;
    uc.reconnect_max = config.reconnect_max;
    binance::UserStreamHandlers uh;
    uh.on_live = [this] { on_live(); };
    uh.on_down = [this](const std::string& reason) { on_down(reason); };
    uh.on_frame = [this](std::span<const std::byte> f, std::int64_t recv) { on_frame(f, recv); };
    stream = std::make_unique<binance::UserStreamSession>(io, std::move(uc), std::move(uh));
    retry = std::make_unique<network::Timer>(io);
    check_timer = std::make_unique<network::Timer>(io);
    schedule_check();
    api->start();
  }

  // The commands whose records are durable (all of them without a barrier); `held` is set when
  // the head of the ring still waits for the persist thread.
  std::size_t drain(bool& held) {
    std::size_t n = 0;
    held = false;
    while (const QueuedCommand* q = commands.front()) {
      if (q->durable_at > 0 && config.durable != nullptr &&
          config.durable->load(std::memory_order_acquire) < q->durable_at) {
        if (!head_held) {
          Counters::add(counters.barrier_waits);
          head_held = true;
        }
        held = true;
        break;
      }
      const VenueCommand c = q->command;
      const std::uint64_t queued = q->queued_ns;
      commands.pop();
      head_held = false;
      command(c);
      if (queued != 0) {
        command_latency.add(steady_now_ns() - queued);
      }
      ++n;
    }
    return n;
  }

  void run() {
    while (!stopping.load(std::memory_order_relaxed)) {
      bool held = false;
      std::size_t work = drain(held);
      work += io.poll();
      if (work == 0) {
        if (config.busy_poll || held) {
          std::this_thread::yield();
        } else {
          io.run_for(std::chrono::milliseconds{1});
        }
      }
      io.restart();
    }
    bool held = false;
    drain(held); // the commands the core sent before stopping (whose records are durable)
    counters.unsent_at_stop.store(commands.size(), std::memory_order_relaxed);
    io.run_for(std::chrono::milliseconds{200}); // let them and the closes go out
  }

  void close_all() const {
    retry->cancel();
    check_timer->cancel();
    stream->stop();
    api->stop();
  }
};

VenueIo::VenueIo(const ArrivalClock& clock, VenueIoConfig config)
    : impl_{std::make_unique<Impl>(clock, std::move(config))} {}

VenueIo::~VenueIo() { stop(); }

Status VenueIo::start(std::string& error) {
  Impl& v = *impl_;
  if (v.thread.joinable()) {
    return Status::Ok;
  }
  if (!v.config.raw_frames.empty()) {
    const Status s = v.raw.open(v.config.raw_frames, error);
    if (!core::ok(s)) {
      return s;
    }
    v.recording = true;
  }
  v.open();
  v.thread = std::thread{[&v] { v.run(); }};
  v.rest.start([&v](binance::RestClient& client) { v.key_tick(client); });
  v.rest.post([&v](binance::RestClient& client) { v.key_tick(client); }); // the key at once
  return Status::Ok;
}

void VenueIo::stop() {
  Impl& v = *impl_;
  if (!v.thread.joinable()) {
    return;
  }
  // The IO thread first: its last commands may hand the REST thread a countdownCancelAll, which
  // the REST thread still sends when it stops.
  v.io.post([&v] { v.close_all(); });
  v.stopping.store(true);
  v.thread.join();
  v.rest.stop();
  if (v.recording) {
    static_cast<void>(v.raw.close());
    v.recording = false;
  }
}

SpscByteRing& VenueIo::ring() noexcept { return impl_->ring; }
SpscRing<QueuedCommand>& VenueIo::commands() noexcept { return impl_->commands; }
std::uint16_t VenueIo::source_id() const noexcept { return impl_->config.source_id; }

std::string VenueIo::last_error() const {
  const std::lock_guard lock{impl_->error_mutex};
  return impl_->error;
}

VenueIoStats VenueIo::stats() const noexcept {
  const Counters& c = impl_->counters;
  const auto v = [](const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };
  return VenueIoStats{v(c.events),
                      v(c.ring_waits),
                      v(c.commands),
                      v(c.refused_locally),
                      v(c.unknown_outcomes),
                      v(c.frames),
                      v(c.decode_errors),
                      v(c.snapshots),
                      v(c.snapshot_failures),
                      v(c.stale_snapshots),
                      v(c.key_failures),
                      v(c.countdowns),
                      v(c.countdown_failures),
                      v(c.checks),
                      v(c.check_failures),
                      v(c.rest_orders),
                      v(c.barrier_waits),
                      v(c.unsent_at_stop),
                      impl_->rest.client().too_many_requests(),
                      impl_->rest.client().bans()};
}

HistogramData VenueIo::command_latency() const noexcept { return impl_->command_latency.read(); }

} // namespace jarvis::live
