#include "jarvis/live/market_feed.hpp"

#include <thread>
#include <utility>

#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/binance/rest_codec.hpp"
#include "jarvis/adapter/binance/streams.hpp"
#include "jarvis/adapter/binance/ws_api.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/network/backoff.hpp"
#include "jarvis/network/timer.hpp"
#include "jarvis/network/ws_client.hpp"

namespace jarvis::live {

namespace {

using core::Status;
namespace binance = adapter::binance;

constexpr std::chrono::milliseconds kSnapshotTick{100};

std::span<const std::byte> bytes_of(std::string_view s) {
  return std::as_bytes(std::span<const char>{s.data(), s.size()});
}

struct Counters {
  std::atomic<std::uint64_t> messages{0};
  std::atomic<std::uint64_t> events{0};
  std::atomic<std::uint64_t> decode_errors{0};
  std::atomic<std::uint64_t> unsupported{0};
  std::atomic<std::uint64_t> ring_waits{0};
  std::atomic<std::uint64_t> connects{0};
  std::atomic<std::uint64_t> snapshots{0};
  std::atomic<std::uint64_t> snapshot_failures{0};
  std::atomic<std::uint64_t> book_syncs{0};

  static void add(std::atomic<std::uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }
};

} // namespace

struct MarketFeed::Impl final : adapter::EventEmitter {
  struct Conn {
    binance::StreamConnection spec;
    std::uint32_t id = 0;
    bool depth = false; // carries the diff streams
    std::unique_ptr<network::WsClient> ws;
    std::unique_ptr<network::Timer> reconnect;
    network::Backoff backoff;
    bool open = false;
  };

  Impl(const ArrivalClock& clock_, MarketFeedConfig config_)
      : clock{&clock_}, config{std::move(config_)}, ring{config.ring_bytes}, codec{config.symbols},
        books{config.symbols, binance::DepthSyncConfig{4096, config.max_levels}},
        scratch(model::wire::kRecordHeaderSize + model::wire::kMaxPayload +
                model::wire::kRecordTrailerSize) {}

  const ArrivalClock* clock;
  MarketFeedConfig config;
  SpscByteRing ring;
  Counters counters;
  network::IoContext io;
  binance::JsonCodec codec;
  binance::DepthBooks books;
  std::vector<std::byte> scratch;
  std::vector<std::unique_ptr<Conn>> conns;
  std::unique_ptr<binance::WsApiSession> api;
  std::unique_ptr<network::Timer> snapshot_timer;
  std::vector<binance::SnapshotRequest> due;
  RawFrameWriter raw;
  bool recording = false;
  std::uint32_t api_conn = 0;
  std::size_t open_streams = 0; // stream connections open
  bool market_up = false;       // all of them, as last recorded
  std::atomic<bool> stopping{false};
  std::thread thread;

  [[nodiscard]] core::UnixNanos utc(std::int64_t steady) const {
    return core::UnixNanos{clock->utc_of(steady)};
  }

  void record(RawKind kind, std::uint32_t conn, std::uint8_t opcode, std::string_view text,
              std::uint64_t recv) {
    if (recording) {
      static_cast<void>(raw.write(RawFrame{recv, conn, kind, opcode, bytes_of(text)}));
    }
  }

  // EventEmitter: one normalised event into the ring, waiting while the ring is full.
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
  Status depth(const adapter::DepthDiff& d) override { return books.on_diff(d, *this); }

  // ConnectionStatus(MarketData): up once every stream connection is open, down when one closes.
  void market_data(bool up) {
    if (up == market_up) {
      return;
    }
    market_up = up;
    model::ConnectionStatus c;
    c.venue = config.venue;
    c.kind = model::ConnectionKind::MarketData;
    c.up = up;
    c.ts_init = core::UnixNanos{clock->now()};
    static_cast<void>(event(model::Event{c}));
  }

  void on_message(Conn& c, network::WsOpcode op, std::span<const std::byte> payload,
                  std::int64_t recv) {
    Counters::add(counters.messages);
    const core::UnixNanos at = utc(recv);
    record(RawKind::Message, c.id, static_cast<std::uint8_t>(op),
           {reinterpret_cast<const char*>(payload.data()), payload.size()}, // NOLINT
           at.value());
    adapter::ConnCtx ctx{c.id, at};
    const Status s = codec.decode(payload, ctx, *this);
    if (s == Status::UnsupportedMessage) {
      Counters::add(counters.unsupported);
    } else if (!core::ok(s)) {
      Counters::add(counters.decode_errors);
    }
  }

  void on_close(Conn& c, const std::string& reason) {
    const core::UnixNanos now{clock->now()};
    record(RawKind::Close, c.id, 0, reason, now.value());
    if (c.open) {
      c.open = false;
      --open_streams;
      market_data(false);
    }
    if (c.depth) {
      static_cast<void>(books.disconnected(now, *this));
    }
    if (stopping.load()) {
      return;
    }
    Conn* conn = &c;
    c.reconnect->after(c.backoff.next(), [conn] { conn->ws->connect(); });
  }

  void open_connections() {
    const std::vector<binance::StreamConnection> specs =
        binance::stream_connections(config.endpoints.streams, config.streams);
    for (const binance::StreamConnection& spec : specs) {
      auto c = std::make_unique<Conn>(
          Conn{spec, static_cast<std::uint32_t>(conns.size()), false, nullptr, nullptr,
               network::Backoff{config.reconnect_initial, config.reconnect_max, conns.size()}});
      for (const std::string& s : spec.streams) {
        c->depth = c->depth || s.find("@depth") != std::string::npos;
      }
      Conn* conn = c.get();
      network::WsHandlers h;
      h.on_open = [this, conn] {
        Counters::add(counters.connects);
        record(RawKind::Open, conn->id, 0, conn->spec.url, clock->now());
        conn->backoff.reset();
        if (conn->depth) {
          books.connected();
        }
        conn->open = true;
        ++open_streams;
        if (open_streams == conns.size()) {
          market_data(true);
        }
      };
      h.on_message = [this, conn](network::WsOpcode op, std::span<const std::byte> payload,
                                  std::int64_t recv) { on_message(*conn, op, payload, recv); };
      h.on_close = [this, conn](const std::string& reason) { on_close(*conn, reason); };
      network::WsConfig wc;
      wc.url = spec.url;
      wc.tls = config.endpoints.tls;
      c->ws = std::make_unique<network::WsClient>(io, std::move(wc), std::move(h));
      c->reconnect = std::make_unique<network::Timer>(io);
      conns.push_back(std::move(c));
    }
    api_conn = static_cast<std::uint32_t>(conns.size());
    binance::WsApiConfig ac;
    ac.url = config.endpoints.ws_api;
    ac.tls = config.endpoints.tls;
    ac.reconnect_initial = config.reconnect_initial;
    ac.reconnect_max = config.reconnect_max;
    binance::WsApiHandlers ah;
    ah.on_ready = [this] {
      record(RawKind::Open, api_conn, 0, config.endpoints.ws_api, clock->now());
    };
    ah.on_down = [this](const std::string& reason) {
      record(RawKind::Close, api_conn, 0, reason, clock->now());
    };
    api = std::make_unique<binance::WsApiSession>(io, std::move(ac), std::move(ah));
    snapshot_timer = std::make_unique<network::Timer>(io);
    for (const auto& c : conns) {
      c->ws->connect();
    }
    api->start();
    arm_snapshots();
  }

  void arm_snapshots() {
    snapshot_timer->after(kSnapshotTick, [this] {
      request_snapshots();
      std::uint64_t syncs = 0;
      for (std::uint32_t i = 0; i < books.size(); ++i) {
        syncs += books[i].stats().syncs;
      }
      counters.book_syncs.store(syncs, std::memory_order_relaxed);
      if (!stopping.load()) {
        arm_snapshots();
      }
    });
  }

  void request_snapshots() {
    if (!api->ready()) {
      return; // the books wait in Buffering until the WebSocket API is up
    }
    books.due_snapshots(core::UnixNanos{clock->now()}, due);
    for (const binance::SnapshotRequest r : due) {
      std::string error;
      std::string sent;
      const Status s = api->request(
          "depth",
          {{"symbol", config.names[r.symbol]}, {"limit", std::to_string(config.snapshot_limit)}},
          binance::Security::None,
          [this, r](Status answered, const binance::WsAnswer& a, std::string_view json) {
            on_snapshot(r, answered, a, json);
          },
          error, &sent);
      if (core::ok(s)) {
        record(RawKind::Sent, api_conn, 1, sent, clock->now()); // pairs the answer with its symbol
      } else {
        books.snapshot_failed(r);
        Counters::add(counters.snapshot_failures);
      }
    }
  }

  void on_snapshot(const binance::SnapshotRequest& r, Status answered, const binance::WsAnswer& a,
                   std::string_view json) {
    const core::UnixNanos recv{clock->now()};
    if (!core::ok(answered) || a.status != 200) {
      books.snapshot_failed(r);
      Counters::add(counters.snapshot_failures);
      return;
    }
    record(RawKind::Message, api_conn, 1, json, recv.value());
    std::string id;
    std::string error;
    binance::DepthSnapshot snap;
    if (!core::ok(binance::decode_ws_depth_response(json, config.symbols[r.symbol], recv, id, snap,
                                                    error))) {
      books.snapshot_failed(r);
      Counters::add(counters.snapshot_failures);
      return;
    }
    Counters::add(counters.snapshots);
    static_cast<void>(books.on_snapshot(r, snap, *this));
  }

  void run() {
    while (!stopping.load()) {
      io.run_for(std::chrono::milliseconds{50});
      io.restart();
    }
    // Let the closes go out.
    io.run_for(std::chrono::milliseconds{200});
  }

  void close_all() {
    snapshot_timer->cancel();
    for (const auto& c : conns) {
      c->reconnect->cancel();
      c->ws->close();
    }
    api->stop();
  }
};

MarketFeed::MarketFeed(const ArrivalClock& clock, MarketFeedConfig config)
    : impl_{std::make_unique<Impl>(clock, std::move(config))} {}

MarketFeed::~MarketFeed() { stop(); }

Status MarketFeed::start(std::string& error) {
  Impl& m = *impl_;
  if (m.thread.joinable()) {
    return Status::Ok;
  }
  if (!m.config.raw_frames.empty()) {
    const Status s = m.raw.open(m.config.raw_frames, error);
    if (!core::ok(s)) {
      return s;
    }
    m.recording = true;
  }
  m.open_connections();
  m.thread = std::thread{[&m] { m.run(); }};
  return Status::Ok;
}

void MarketFeed::stop() {
  Impl& m = *impl_;
  if (!m.thread.joinable()) {
    return;
  }
  m.io.post([&m] { m.close_all(); });
  m.stopping.store(true);
  m.thread.join();
  if (m.recording) {
    static_cast<void>(m.raw.close());
    m.recording = false;
  }
}

SpscByteRing& MarketFeed::ring() noexcept { return impl_->ring; }

std::uint16_t MarketFeed::source_id() const noexcept { return impl_->config.source_id; }

MarketFeedStats MarketFeed::stats() const noexcept {
  const Counters& c = impl_->counters;
  const auto v = [](const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };
  return MarketFeedStats{v(c.messages),   v(c.events),   v(c.decode_errors), v(c.unsupported),
                         v(c.ring_waits), v(c.connects), v(c.snapshots),     v(c.snapshot_failures),
                         v(c.book_syncs)};
}

} // namespace jarvis::live
