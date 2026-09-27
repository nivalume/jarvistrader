// jarvis-capture: records WebSocket streams into a raw frame file (docs/architecture.md section
// 13.4) and prints such files. It captures codec fixtures and fuzz corpora today and is the
// seed of the live recorder (plan.md M4).
//
//   jarvis-capture record --url URL [--url URL ...] --seconds N --out FILE
//   jarvis-capture dump FILE [--jsonl | --timed] [--conn N] [--limit N]
//
//   jarvis-capture decode FILE --exchange-info JSON [--symbols A,B]
//
// `record` reconnects with backoff until the time is up. `dump --jsonl` prints the text
// messages one per line; `--timed` prefixes each with its arrival time ("<recv_ns> <json>"),
// the form the codec fixtures are stored in. `decode` runs the USDⓈ-M JSON codec over a
// capture with instruments from a saved exchangeInfo and reports what it produced; it exits 1
// if any message fails to decode.

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/network/backoff.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/timer.hpp"
#include "jarvis/network/ws_client.hpp"

namespace {

namespace net = jarvis::network;
namespace live = jarvis::live;
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
  const std::array<std::pair<std::string_view, std::optional<std::string>*>, 3> texts{
      {{"--out", &o.out}, {"--exchange-info", &o.exchange_info}, {"--symbols", &o.symbols}}};
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

} // namespace

int main(int argc, char** argv) {
  Options o;
  const std::span<char*> args{argv + 1, static_cast<std::size_t>(argc > 0 ? argc - 1 : 0)};
  if (!parse(args, o) || o.positional.empty()) {
    std::fprintf(stderr, "usage: jarvis-capture record|dump|decode ...\n");
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
  std::fprintf(stderr, "unknown command %s\n", o.positional[0].c_str());
  return 2;
}
