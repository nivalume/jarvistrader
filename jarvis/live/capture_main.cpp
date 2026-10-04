// jarvis-capture: records WebSocket streams into a raw frame file (docs/architecture.md section
// 13.4) and prints such files. It captures codec fixtures and fuzz corpora today and is the
// seed of the live recorder (plan.md M4).
//
//   jarvis-capture record --url URL [--url URL ...] --seconds N --out FILE
//   jarvis-capture dump FILE [--jsonl | --timed] [--conn N] [--limit N]
//
//   jarvis-capture decode FILE --exchange-info JSON [--symbols A,B]
//   jarvis-capture depth-check --exchange-info JSON --symbols SYMBOL --seconds N
//   jarvis-capture ws-api-probe [--url URL] [--symbols SYMBOL]
//   jarvis-capture redecode FILE --exchange-info JSON [--symbols A,B] [--out DIR] [--check RUN]
//
// `record` reconnects with backoff until the time is up. `dump --jsonl` prints the text
// messages one per line; `--timed` prefixes each with its arrival time ("<recv_ns> <json>"),
// the form the codec fixtures are stored in. `decode` runs the USDⓈ-M JSON codec over a
// capture with instruments from a saved exchangeInfo and reports what it produced; it exits 1
// if any message fails to decode. `depth-check` syncs the production depth stream with snapshots
// from the WebSocket API and, every 15 seconds, syncs a second book from a fresh snapshot on
// the same stream; when both reach the same update id their top 100 levels per side must be
// equal. It exits 1 on any mismatch or if no comparison was made. `ws-api-probe` checks the
// WebSocket API session against the venue with its public methods (time, depth) and prints the
// answers and rate limits; it exits 1 unless both are answered. `redecode` rebuilds the decoded
// market data of a raw frame file (a sandbox run's raw-frames.jraw): the same codec and depth
// sync over the same frames in the same order, snapshot answers paired with their symbols by
// the recorded requests. --out writes the events as an event log; --check requires the market
// data inputs of a run log to be exactly the first redecoded events, in order and byte for byte
// (exit 1 on a difference).

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/binance/rest_codec.hpp"
#include "jarvis/adapter/binance/ws_api.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/redecode.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/network/backoff.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/timer.hpp"
#include "jarvis/network/ws_client.hpp"
#include "jarvis/node/build_info.hpp"
#include "jarvis/node/event_log.hpp"
#include <algorithm>

namespace {

namespace net = jarvis::network;
namespace live = jarvis::live;
namespace wire = jarvis::model::wire;
using jarvis::core::ok;
using jarvis::core::Status;

std::atomic<bool> g_interrupted{false};

extern "C" void on_signal(int /*signal*/) { g_interrupted = true; }

struct Options {
  std::vector<std::string> positional;
  std::vector<std::string> urls;
  std::optional<std::string> out;
  std::optional<std::string> exchange_info;
  std::optional<std::string> symbols;
  std::optional<std::string> check;
  std::optional<long> seconds;
  std::optional<long> conn;
  std::optional<long> limit;
  bool jsonl = false;
  bool timed = false;
};

bool parse_number(std::string_view text, long& out) {
  try {
    std::size_t used = 0;
    out = std::stol(std::string{text}, &used);
    return used == text.size() && out >= 0;
  } catch (const std::exception&) {
    return false;
  }
}

// Sets the option `name` from `value`: --url adds, the others replace.
bool set_option(std::string_view name, std::string_view value, Options& o) {
  const std::array<std::pair<std::string_view, std::optional<std::string>*>, 4> texts{
      {{"--out", &o.out},
       {"--exchange-info", &o.exchange_info},
       {"--symbols", &o.symbols},
       {"--check", &o.check}}};
  const std::array<std::pair<std::string_view, std::optional<long>*>, 3> numbers{
      {{"--seconds", &o.seconds}, {"--conn", &o.conn}, {"--limit", &o.limit}}};
  if (name == "--url") {
    o.urls.emplace_back(value);
    return true;
  }
  for (const auto& [option, target] : texts) {
    if (name == option) {
      *target = std::string{value};
      return true;
    }
  }
  for (const auto& [option, target] : numbers) {
    if (name == option) {
      long n = 0;
      if (!parse_number(value, n)) {
        std::fprintf(stderr, "%s needs a non-negative number\n", std::string{name}.c_str());
        return false;
      }
      *target = n;
      return true;
    }
  }
  std::fprintf(stderr, "unknown option %s\n", std::string{name}.c_str());
  return false;
}

bool parse(std::span<char*> args, Options& o) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--jsonl") {
      o.jsonl = true;
    } else if (a == "--timed") {
      o.timed = true;
    } else if (!a.starts_with("--")) {
      o.positional.emplace_back(a);
    } else if (i + 1 >= args.size()) {
      std::fprintf(stderr, "%s needs a value\n", std::string{a}.c_str());
      return false;
    } else if (!set_option(a, args[++i], o)) {
      return false;
    }
  }
  return true;
}

std::span<const std::byte> bytes_of(std::string_view text) {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

struct Connection {
  Connection(net::IoContext& io, std::uint32_t id, std::string u)
      : conn_id{id}, url{std::move(u)}, timer{io},
        backoff{std::chrono::milliseconds{500}, std::chrono::seconds{30}, id + 1} {}
  std::uint32_t conn_id;
  std::string url;
  std::unique_ptr<net::WsClient> client;
  net::Timer timer;
  net::Backoff backoff;
  std::uint64_t messages = 0;
  std::uint64_t reconnects = 0;
};

int record(const Options& o) {
  if (o.urls.empty() || !o.out || !o.seconds) {
    std::fprintf(stderr, "usage: jarvis-capture record --url URL [--url URL ...] --seconds N "
                         "--out FILE\n");
    return 2;
  }
  live::RawFrameWriter writer;
  std::string error;
  if (!ok(writer.open(*o.out, error))) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const live::ArrivalClock clock;
  net::IoContext io;
  bool stopping = false;
  bool write_failed = false;
  const auto put = [&](const Connection& c, live::RawKind kind, std::uint64_t ns,
                       std::uint8_t opcode, std::span<const std::byte> bytes) {
    if (!ok(writer.write(live::RawFrame{ns, c.conn_id, kind, opcode, bytes}))) {
      write_failed = true;
      io.stop();
    }
  };
  std::vector<std::unique_ptr<Connection>> conns;
  for (const std::string& url : o.urls) {
    auto c = std::make_unique<Connection>(io, static_cast<std::uint32_t>(conns.size()), url);
    Connection* self = c.get();
    net::WsConfig config;
    config.url = url;
    config.idle_timeout = std::chrono::seconds{30}; // a recording must not hang on a dead flow
    net::WsHandlers h;
    h.on_open = [&, self] {
      self->backoff.reset();
      put(*self, live::RawKind::Open, clock.now(), 0, bytes_of(self->url));
    };
    h.on_message = [&, self](net::WsOpcode op, std::span<const std::byte> payload,
                             std::int64_t recv_ns) {
      ++self->messages;
      put(*self, live::RawKind::Message, clock.utc_of(recv_ns), static_cast<std::uint8_t>(op),
          payload);
    };
    h.on_close = [&, self](const std::string& reason) {
      put(*self, live::RawKind::Close, clock.now(), 0, bytes_of(reason));
      std::fprintf(stderr, "connection %u closed: %s\n", self->conn_id, reason.c_str());
      if (!stopping) {
        ++self->reconnects;
        self->timer.after(self->backoff.next(), [self] { self->client->connect(); });
      }
    };
    c->client = std::make_unique<net::WsClient>(io, config, h);
    conns.push_back(std::move(c));
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  for (const auto& c : conns) {
    c->client->connect();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{*o.seconds};
  while (!g_interrupted && !write_failed && std::chrono::steady_clock::now() < deadline) {
    io.run_for(std::chrono::milliseconds{200});
  }
  stopping = true;
  for (const auto& c : conns) {
    c->timer.cancel();
    c->client->close();
  }
  io.run_for(std::chrono::seconds{3}); // the closing handshakes
  conns.clear();
  io.run_for(std::chrono::milliseconds{100});
  const Status closed = writer.close();
  for (std::size_t i = 0; i < o.urls.size(); ++i) {
    std::printf("connection %zu: %s\n", i, o.urls[i].c_str());
  }
  std::printf("records %llu, bytes %llu -> %s\n", static_cast<unsigned long long>(writer.records()),
              static_cast<unsigned long long>(writer.bytes()), o.out->c_str());
  if (write_failed || !ok(closed)) {
    std::fprintf(stderr, "writing %s failed\n", o.out->c_str());
    return 1;
  }
  return 0;
}

const char* kind_name(live::RawKind kind) {
  switch (kind) {
  case live::RawKind::Open:
    return "open";
  case live::RawKind::Message:
    return "message";
  case live::RawKind::Close:
    return "close";
  case live::RawKind::Sent:
    return "sent";
  }
  return "?";
}

void print_frame(const Options& o, const live::RawFrame& f) {
  if (o.timed) {
    std::printf("%llu ", static_cast<unsigned long long>(f.recv_ns));
  }
  if (o.jsonl || o.timed) {
    std::fwrite(f.bytes.data(), 1, f.bytes.size(), stdout);
    std::fputc('\n', stdout);
    return;
  }
  std::printf("%llu conn=%u %s len=%zu %.*s\n", static_cast<unsigned long long>(f.recv_ns),
              f.conn_id, kind_name(f.kind), f.bytes.size(),
              static_cast<int>(f.bytes.size() > 200 ? 200 : f.bytes.size()), f.text().data());
}

// Calls `fn` for every frame of the file; false (after a message) when it cannot be read.
template <typename Fn> bool for_each_frame(const std::string& path, Fn&& fn) {
  live::RawFrameReader reader;
  std::string error;
  if (!ok(reader.open(path, error))) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  live::RawFrame f;
  for (;;) {
    const Status s = reader.next(f);
    if (s == Status::EndOfStream) {
      return true;
    }
    if (!ok(s)) {
      std::fprintf(stderr, "%s: %s at offset %zu\n", path.c_str(),
                   std::string{jarvis::core::to_string(s)}.c_str(), reader.offset());
      return false;
    }
    if (!fn(f)) {
      return true;
    }
  }
}

int dump(const Options& o) {
  if (o.positional.size() != 2) {
    std::fprintf(stderr,
                 "usage: jarvis-capture dump FILE [--jsonl | --timed] [--conn N] [--limit N]\n");
    return 2;
  }
  long printed = 0;
  const bool read = for_each_frame(o.positional[1], [&](const live::RawFrame& f) {
    if ((o.conn && f.conn_id != static_cast<std::uint32_t>(*o.conn)) ||
        ((o.jsonl || o.timed) && f.kind != live::RawKind::Message)) {
      return true;
    }
    if (o.limit && printed >= *o.limit) {
      return false;
    }
    ++printed;
    print_frame(o, f);
    return true;
  });
  return read ? 0 : 1;
}

// Counts decoded events by type and checks depth diff continuity per symbol (pu == previous u),
// a preview of the synchronizer's gap detection.
class DecodeReport final : public jarvis::adapter::EventEmitter {
public:
  Status event(const jarvis::model::Event& e) override {
    static constexpr std::array<const char*, 10> kNames = {
        "TradeTick",       "QuoteTick",        "OrderBookDeltas",   "Bar",
        "MarkPriceUpdate", "IndexPriceUpdate", "FundingRateUpdate", "InstrumentStatus",
        "InstrumentClose", "LiquidationOrder"};
    ++counts[e.index() < kNames.size() ? kNames[e.index()] : "other"];
    return Status::Ok;
  }
  Status depth(const jarvis::adapter::DepthDiff& d) override {
    ++counts["DepthDiff"];
    std::uint64_t& last = last_u[d.symbol];
    if (last != 0 && d.prev_final_update_id != last) {
      ++gaps;
    }
    last = d.final_update_id;
    return Status::Ok;
  }
  std::map<std::string, std::uint64_t> counts;
  std::map<std::uint32_t, std::uint64_t> last_u;
  std::uint64_t gaps = 0;
};

std::vector<std::string> split_symbols(const std::optional<std::string>& list) {
  std::vector<std::string> out;
  const std::string all = list.value_or("");
  std::string_view rest = all;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    out.emplace_back(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  return out;
}

// The symbol table of the instruments named by --symbols (every perpetual without it).
bool load_symbols(const std::string& path, const std::optional<std::string>& symbols,
                  jarvis::adapter::SymbolTable& table) {
  std::ifstream in{path, std::ios::binary};
  std::ostringstream json;
  json << in.rdbuf();
  std::vector<jarvis::adapter::binance::PerpetualDefinition> defs;
  std::string error;
  if (!ok(jarvis::adapter::binance::parse_exchange_info(json.str(), split_symbols(symbols),
                                                        jarvis::core::UnixNanos{}, defs, error))) {
    std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
    return false;
  }
  for (const auto& d : defs) {
    if (!ok(table.add(d.instrument.common))) {
      std::fprintf(stderr, "duplicate symbol in exchangeInfo\n");
      return false;
    }
  }
  return true;
}

void print_report(const jarvis::adapter::binance::CodecStats& st, const DecodeReport& report,
                  std::size_t instruments) {
  const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("instruments %zu, messages %llu, events %llu, depth diffs %llu, depth gaps %llu\n",
              instruments, u(st.messages), u(st.events), u(st.depth_diffs), u(report.gaps));
  std::printf("unknown symbol %llu, stale quotes %llu, open klines %llu, control %llu, "
              "unsupported %llu, errors %llu\n",
              u(st.unknown_symbol), u(st.stale_quotes), u(st.open_klines), u(st.control),
              u(st.unsupported), u(st.errors));
  for (const auto& [name, count] : report.counts) {
    std::printf("  %-18s %llu\n", name.c_str(), u(count));
  }
}

int decode(const Options& o) {
  if (o.positional.size() != 2 || !o.exchange_info) {
    std::fprintf(stderr,
                 "usage: jarvis-capture decode FILE --exchange-info JSON [--symbols A,B]\n");
    return 2;
  }
  jarvis::adapter::SymbolTable table;
  if (!load_symbols(*o.exchange_info, o.symbols, table)) {
    return 1;
  }
  jarvis::adapter::binance::JsonCodec codec{table};
  DecodeReport report;
  std::uint64_t reported = 0;
  const bool read = for_each_frame(o.positional[1], [&](const live::RawFrame& f) {
    if (f.kind != live::RawKind::Message) {
      return true;
    }
    jarvis::adapter::ConnCtx conn{f.conn_id, jarvis::core::UnixNanos{f.recv_ns}};
    const Status d = codec.decode(f.bytes, conn, report);
    if (!ok(d) && reported++ < 10) {
      std::fprintf(stderr, "%s: %s in %.*s\n", std::string{jarvis::core::to_string(d)}.c_str(),
                   codec.error().c_str(),
                   static_cast<int>(f.bytes.size() > 160 ? 160 : f.bytes.size()), f.text().data());
    }
    return true;
  });
  if (!read) {
    return 1;
  }
  print_report(codec.stats(), report, table.size());
  return codec.stats().errors == 0 && codec.stats().unsupported == 0 ? 0 : 1;
}

// ---- depth-check ---------------------------------------------------------------------------

namespace binance = jarvis::adapter::binance;

class NullEmitter final : public jarvis::adapter::EventEmitter {
public:
  Status event(const jarvis::model::Event& /*e*/) override {
    ++batches;
    return Status::Ok;
  }
  Status depth(const jarvis::adapter::DepthDiff& /*d*/) override { return Status::Ok; }
  std::uint64_t batches = 0;
};

template <typename Map> bool same_top(const Map& a, const Map& b, std::size_t levels) {
  auto x = a.begin();
  auto y = b.begin();
  for (std::size_t i = 0; i < levels && x != a.end() && y != b.end(); ++i, ++x, ++y) {
    if (x->first != y->first || x->second.size.raw() != y->second.size.raw()) {
      return false;
    }
  }
  return true;
}

struct DepthCheck final : jarvis::adapter::EventEmitter {
  static constexpr std::size_t kLevels = 100;

  explicit DepthCheck(const jarvis::model::InstrumentId& id) : primary{id}, checker{id} {}

  Status event(const jarvis::model::Event& /*e*/) override { return Status::Ok; }
  Status depth(const jarvis::adapter::DepthDiff& d) override {
    Status s = primary.on_diff(d, sink);
    if (checking) {
      const Status c = checker.on_diff(d, sink);
      s = ok(s) ? c : s;
      compare(d.ts_init);
    }
    return s;
  }

  void compare(jarvis::core::UnixNanos ts) {
    if (!primary.visible() || !checker.visible() ||
        primary.last_update_id() != checker.last_update_id()) {
      return;
    }
    ++checks;
    const bool equal = same_top(primary.bids(), checker.bids(), kLevels) &&
                       same_top(primary.asks(), checker.asks(), kLevels);
    if (!equal) {
      ++mismatches;
    }
    std::printf("check %llu at update %llu: %s (%zu bids, %zu asks)\n",
                static_cast<unsigned long long>(checks),
                static_cast<unsigned long long>(primary.last_update_id()),
                equal ? "equal" : "MISMATCH", primary.bids().size(), primary.asks().size());
    static_cast<void>(checker.disconnected(ts, sink));
    checking = false;
  }

  // Registers a snapshot request for `sync`; returns the WebSocket API request id.
  std::string request(binance::DepthSync& sync) {
    const std::uint64_t req = sync.snapshot_requested();
    std::string id = "d" + std::to_string(++next_id);
    pending[id] = {&sync, req};
    return id;
  }

  // An answer of the WebSocket API: hands the snapshot to the book that asked for it.
  void on_answer(std::string_view text, const jarvis::adapter::SymbolEntry& entry,
                 jarvis::core::UnixNanos recv) {
    std::string id;
    std::string error;
    binance::DepthSnapshot snap;
    const Status s = binance::decode_ws_depth_response(text, entry, recv, id, snap, error);
    const auto it = pending.find(id);
    if (it == pending.end()) {
      std::fprintf(stderr, "an answer to no request: %s\n", error.c_str());
      return;
    }
    const auto [sync, req] = it->second;
    pending.erase(it);
    if (!ok(s)) {
      std::fprintf(stderr, "snapshot %s: %s\n", id.c_str(), error.c_str());
      sync->snapshot_failed(req);
      return;
    }
    static_cast<void>(sync->on_snapshot(req, snap, sink));
  }

  // Requests the snapshots due now through `send(id)`, and starts a check every 15 seconds.
  template <typename Send> void poll(std::chrono::steady_clock::time_point now, Send&& send) {
    if (primary.wants_snapshot()) {
      send(request(primary));
    }
    if (!checking && primary.visible() && now >= next_check) {
      checker.connected();
      checking = true;
      next_check = now + std::chrono::seconds{15};
    }
    if (checking && checker.wants_snapshot()) {
      send(request(checker));
    }
  }

  binance::DepthSync primary;
  binance::DepthSync checker;
  NullEmitter sink;
  bool checking = false;
  std::uint64_t checks = 0;
  std::uint64_t mismatches = 0;
  std::map<std::string, std::pair<binance::DepthSync*, std::uint64_t>> pending;
  std::uint64_t next_id = 0;
  std::chrono::steady_clock::time_point next_check =
      std::chrono::steady_clock::now() + std::chrono::seconds{15};
};

int depth_check(const Options& o) {
  if (!o.exchange_info || !o.symbols || !o.seconds) {
    std::fprintf(stderr, "usage: jarvis-capture depth-check --exchange-info JSON --symbols SYMBOL "
                         "--seconds N\n");
    return 2;
  }
  jarvis::adapter::SymbolTable table;
  if (!load_symbols(*o.exchange_info, o.symbols, table) || table.size() != 1) {
    std::fprintf(stderr, "depth-check needs exactly one symbol\n");
    return 2;
  }
  const jarvis::adapter::SymbolEntry entry = table[0];
  const std::string symbol = split_symbols(o.symbols).front();
  std::string lower = symbol;
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  net::IoContext io;
  const live::ArrivalClock clock;
  binance::JsonCodec codec{table};
  DepthCheck check{entry.id};
  bool failed = false;
  bool stopping = false; // our own closes at the end are not failures

  net::WsHandlers md_handlers;
  md_handlers.on_open = [&] { check.primary.connected(); };
  md_handlers.on_message = [&](net::WsOpcode, std::span<const std::byte> payload,
                               std::int64_t recv) {
    jarvis::adapter::ConnCtx conn{0, jarvis::core::UnixNanos{clock.utc_of(recv)}};
    if (!ok(codec.decode(payload, conn, check))) {
      std::fprintf(stderr, "decode: %s\n", codec.error().c_str());
    }
  };
  md_handlers.on_close = [&](const std::string& reason) {
    if (!stopping) {
      std::fprintf(stderr, "depth stream closed: %s\n", reason.c_str());
      failed = true;
    }
  };
  net::WsConfig md_config;
  md_config.url = "wss://fstream.binance.com/public/stream?streams=" + lower + "@depth@100ms";
  net::WsClient md{io, md_config, md_handlers};

  net::WsHandlers api_handlers;
  api_handlers.on_message = [&](net::WsOpcode, std::span<const std::byte> payload,
                                std::int64_t recv) {
    check.on_answer({reinterpret_cast<const char*>(payload.data()), payload.size()}, // NOLINT
                    entry, jarvis::core::UnixNanos{clock.utc_of(recv)});
  };
  api_handlers.on_close = [&](const std::string& reason) {
    if (!stopping) {
      std::fprintf(stderr, "WebSocket API closed: %s\n", reason.c_str());
      failed = true;
    }
  };
  net::WsConfig api_config;
  api_config.url = "wss://ws-fapi.binance.com/ws-fapi/v1";
  net::WsClient api{io, api_config, api_handlers};

  const auto send = [&](const std::string& id) {
    api.send_text(R"({"id":")" + id + R"(","method":"depth","params":{"symbol":")" + symbol +
                  R"(","limit":1000}})");
  };
  net::Timer tick{io};
  std::function<void()> on_tick = [&] {
    if (api.is_open()) {
      check.poll(std::chrono::steady_clock::now(), send);
    }
    tick.after(std::chrono::milliseconds{100}, on_tick);
  };
  std::signal(SIGINT, on_signal);
  md.connect();
  api.connect();
  tick.after(std::chrono::milliseconds{100}, on_tick);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{*o.seconds};
  while (!g_interrupted && !failed && std::chrono::steady_clock::now() < deadline) {
    io.run_for(std::chrono::milliseconds{200});
  }
  stopping = true;
  tick.cancel();
  md.close();
  api.close();
  io.run_for(std::chrono::seconds{2});
  const binance::DepthSyncStats& st = check.primary.stats();
  const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("primary: syncs %llu, gaps %llu, stale snapshots %llu, dropped %llu, applied %llu, "
              "batches %llu\n",
              u(st.syncs), u(st.gaps), u(st.stale_snapshots), u(st.dropped_diffs), u(st.applied),
              u(check.sink.batches));
  std::printf("checks %llu, mismatches %llu, codec errors %llu\n", u(check.checks),
              u(check.mismatches), u(codec.stats().errors));
  return !failed && check.checks > 0 && check.mismatches == 0 && codec.stats().errors == 0 ? 0 : 1;
}

bool run_until_ready(net::IoContext& io, const binance::WsApiSession& session,
                     const std::string& down, std::chrono::steady_clock::time_point deadline) {
  while (!session.ready() && down.empty() && std::chrono::steady_clock::now() < deadline) {
    io.run_for(std::chrono::milliseconds{50});
  }
  if (!session.ready()) {
    std::fprintf(stderr, "not connected: %s\n", down.empty() ? "timeout" : down.c_str());
    return false;
  }
  return true;
}

int ws_api_probe(const Options& o) {
  net::IoContext io;
  binance::WsApiConfig config;
  if (!o.urls.empty()) {
    config.url = o.urls.front();
  }
  binance::WsApiHandlers handlers;
  std::string down;
  handlers.on_down = [&down](const std::string& reason) { down = reason; };
  handlers.on_limits = [](std::span<const jarvis::model::RateLimitFeedback> limits) {
    for (const jarvis::model::RateLimitFeedback& l : limits) {
      std::printf("  rate limit %s per %llu s: %u of %u\n",
                  l.kind == jarvis::model::RateLimitKind::Orders ? "ORDERS" : "REQUEST_WEIGHT",
                  static_cast<unsigned long long>(l.interval_ns / 1'000'000'000), l.used, l.limit);
    }
  };
  binance::WsApiSession session{io, config, handlers};
  session.start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
  if (!run_until_ready(io, session, down, deadline)) {
    return 1;
  }
  int answered = 0;
  const auto report = [&answered](const char* what) {
    return [&answered, what](Status s, const binance::WsAnswer& a, std::string_view json) {
      std::printf("%s: %s status %d%s%.*s\n", what, ok(s) ? "answered" : "failed", a.status,
                  json.empty() ? "" : " ",
                  static_cast<int>(std::min<std::size_t>(json.size(), 160)), json.data());
      answered += ok(s) && a.status == 200 ? 1 : 0;
    };
  };
  std::string error;
  const std::string symbol = o.symbols.value_or("BTCUSDT");
  if (!ok(session.request("time", {}, binance::Security::None, report("time"), error)) ||
      !ok(session.request("depth", {{"symbol", symbol}, {"limit", "5"}}, binance::Security::None,
                          report("depth"), error))) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  while (answered < 2 && std::chrono::steady_clock::now() < deadline + std::chrono::seconds{10}) {
    io.run_for(std::chrono::milliseconds{50});
  }
  const binance::WsApiStats st = session.stats();
  std::printf("sent %llu, unmatched %llu, decode errors %llu\n",
              static_cast<unsigned long long>(st.sent),
              static_cast<unsigned long long>(st.unmatched),
              static_cast<unsigned long long>(st.decode_errors));
  session.stop();
  io.run_for(std::chrono::milliseconds{500});
  return answered == 2 ? 0 : 1;
}

// ---- redecode ------------------------------------------------------------------------------

int redecode(const Options& o) {
  if (o.positional.size() != 2 || !o.exchange_info) {
    std::fprintf(stderr, "usage: jarvis-capture redecode FILE --exchange-info JSON [--symbols A,B] "
                         "[--out DIR] [--check RUN_DIR]\n");
    return 2;
  }
  jarvis::adapter::SymbolTable table;
  if (!load_symbols(*o.exchange_info, o.symbols, table)) {
    return 2;
  }
  jarvis::node::EventLogWriter writer;
  if (o.out) {
    const jarvis::node::BuildInfo info = jarvis::node::build_info();
    wire::LogHeader header;
    static_cast<void>(decltype(header.jarvis_version)::from(info.version, header.jarvis_version));
    static_cast<void>(decltype(header.git_commit)::from(info.git_commit, header.git_commit));
    if (!ok(writer.open(*o.out, header, {}))) {
      std::fprintf(stderr, "cannot write an event log in %s\n", o.out->c_str());
      return 1;
    }
  }
  live::RedecodeResult result;
  std::string error;
  if (!ok(live::redecode(o.positional[1], table, o.out ? &writer : nullptr, result, error))) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (o.out && !ok(writer.close())) {
    std::fprintf(stderr, "cannot finish the event log in %s\n", o.out->c_str());
    return 1;
  }
  const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
  std::printf("frames %llu, stream messages %llu, events %zu, decode errors %llu, snapshots %llu, "
              "book syncs %llu\n",
              u(result.frames), u(result.messages), result.events.size(), u(result.decode_errors),
              u(result.snapshots), u(result.book_syncs));
  if (o.check) {
    std::size_t compared = 0;
    if (!ok(live::check_market_inputs(*o.check, result.events, compared, error))) {
      std::printf("check: %s\n", error.c_str());
      return 1;
    }
    std::printf("check: the run's %zu market data inputs are identical to the first %zu "
                "redecoded events (%zu more arrived after the run stopped)\n",
                compared, compared, result.events.size() - compared);
  }
  return result.decode_errors == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  Options o;
  const std::span<char*> args{argv + 1, static_cast<std::size_t>(argc > 0 ? argc - 1 : 0)};
  if (!parse(args, o) || o.positional.empty()) {
    std::fprintf(
        stderr, "usage: jarvis-capture record|dump|decode|depth-check|ws-api-probe|redecode ...\n");
    return 2;
  }
  if (o.positional[0] == "record") {
    return record(o);
  }
  if (o.positional[0] == "dump") {
    return dump(o);
  }
  if (o.positional[0] == "decode") {
    return decode(o);
  }
  if (o.positional[0] == "depth-check") {
    return depth_check(o);
  }
  if (o.positional[0] == "ws-api-probe") {
    return ws_api_probe(o);
  }
  if (o.positional[0] == "redecode") {
    return redecode(o);
  }
  std::fprintf(stderr, "unknown command %s\n", o.positional[0].c_str());
  return 2;
}
