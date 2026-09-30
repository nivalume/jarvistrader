// Report-only latency of the live node (docs/architecture.md sections 17.3 and 19.2), measured
// by its own telemetry against a local venue over loopback TLS: bookTicker frames every
// millisecond on the market data stream, and a strategy that answers every quote with one
// command (an order far from the market, then its cancel, in turn).
//
//   latency/live   one run; the counters are the telemetry histograms' percentiles:
//                  tick_to_command   the quote's arrival (feed thread) to its command entering
//                                    the venue-io command ring
//                  command_to_socket the command's wait in the ring to its hand-off to the
//                                    WebSocket API connection (venue-io thread)
//
// The histograms have integer nanosecond buckets (250 ns to 100 ms, doubling, section 19.2), so
// a percentile is reported as the upper bound of the bucket it falls in. JARVIS_LATENCY_QUOTES
// sets the number of quotes (default 5000).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "jarvis/data/subscription.hpp"
#include "jarvis/live/live_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "support/tls_test.hpp"
#include "support/ws_test.hpp"

namespace {

namespace live = jarvis::live;
namespace md = jarvis::model;
namespace st = jarvis::strategy;
namespace node = jarvis::node;
using jarvis::core::Status;
using jarvis::testsupport::HandlerHttpsServer;
using jarvis::testsupport::ScriptedWssServer;
using jarvis::testsupport::TempDir;
using jarvis::testsupport::WsReply;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kBaseMs = 1'700'000'002'000;

md::InstrumentId btc() {
  md::InstrumentId id;
  static_cast<void>(md::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id));
  return id;
}

std::string file_text(const std::string& path) {
  std::ifstream in{path};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string field(const std::string& json, std::string_view key) {
  const std::string open = "\"" + std::string{key} + "\":\"";
  const std::size_t at = json.find(open);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + open.size();
  return json.substr(start, json.find('"', start) - start);
}

std::string response(std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

std::int64_t venue_ms(Clock::time_point start) {
  return kBaseMs +
         std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// A venue that acknowledges orders and cancels at once (and reports them on the user stream),
// answers the startup checks and an empty account, and streams quotes once asked to.
class QuoteVenue {
public:
  QuoteVenue()
      : wss_{16, [this](std::size_t conn, const std::string& m) { return ws(conn, m); }},
        https_{[this](const std::string& r) { return rest(r); }, wss_.ca_file(), wss_.key_file()},
        exchange_info_{file_text(std::string{JARVIS_SOURCE_DIR} +
                                 "/tests/data/binance/exchange_info_testnet.json")} {}
  ~QuoteVenue() {
    streaming_ = false;
    if (pusher_.joinable()) {
      pusher_.join();
    }
  }
  QuoteVenue(const QuoteVenue&) = delete;
  QuoteVenue& operator=(const QuoteVenue&) = delete;

  [[nodiscard]] live::LiveRequest request(const node::NodeConfig& config, const std::string& text,
                                          const TempDir& dir, std::atomic<bool>& stop) const {
    live::LiveRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "latency.toml";
    live::FeedEndpoints market;
    market.streams = wss_.url("");
    market.ws_api = wss_.url("/ws-fapi/v1");
    market.tls.ca_file = wss_.ca_file();
    request.market = market;
    live::VenueEndpoints venue;
    venue.rest = https_.url();
    venue.ws_api = wss_.url("/ws-fapi/v1");
    venue.user_stream = wss_.url("/private/stream");
    venue.tls.ca_file = wss_.ca_file();
    request.venue = venue;
    request.spot_rest = "";
    request.credentials = node::ApiCredentials{"KEY1", "s3cret"};
    request.epoch_file = dir.file("epoch");
    request.stop = &stop;
    request.run_for = std::chrono::seconds{120};
    request.now_ms = [start = start_] { return venue_ms(start); };
    return request;
  }

  // Streams `count` quotes, one every `every`, on the market data connection.
  void stream_quotes(std::size_t count, std::chrono::microseconds every) {
    streaming_ = true;
    pusher_ = std::thread{[this, count, every] {
      auto next = Clock::now();
      for (std::size_t i = 0; i < count && streaming_; ++i) {
        next += every;
        std::this_thread::sleep_until(next);
        const std::lock_guard lock{mutex_};
        if (market_) {
          const std::uint64_t u = ++update_;
          const std::string bid = u % 2 == 0 ? "84000.0" : "84000.1";
          wss_.push(
              *market_,
              WsReply::send(R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":)" +
                            std::to_string(u) + R"(,"s":"BTCUSDT","ps":"BTCUSDT","b":")" + bid +
                            R"(","B":"1.5000","a":"84000.2","A":"2.0000","T":)" +
                            std::to_string(venue_ms(start_)) + R"(,"E":)" +
                            std::to_string(venue_ms(start_)) + "}}"));
        }
      }
    }};
  }
  [[nodiscard]] bool ready() {
    const std::lock_guard lock{mutex_};
    return market_.has_value() && stream_.has_value();
  }

private:
  std::string update_of(std::string_view cid, std::uint64_t id, std::string_view exec) const {
    const std::string t = std::to_string(venue_ms(start_));
    return R"({"stream":"LK1","data":{"e":"ORDER_TRADE_UPDATE","E":)" + t + R"(,"T":)" + t +
           R"(,"o":{"s":"BTCUSDT","c":")" + std::string{cid} +
           R"(","S":"BUY","o":"LIMIT","f":"GTC","q":"0.0010","p":"83000.0","ap":"0","sp":"0","x":")" +
           std::string{exec} + R"(","X":")" + std::string{exec} + R"(","i":)" + std::to_string(id) +
           R"(,"l":"0","z":"0","L":"0","N":"USDT","n":"0","T":)" + t +
           R"(,"t":0,"b":"0","a":"0","m":false,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0"}}})";
  }

  std::vector<WsReply> ws(std::size_t conn, const std::string& m) {
    const std::string target = wss_.target(conn);
    const std::lock_guard lock{mutex_};
    if (target == "/private/stream") {
      if (m.empty()) {
        return {};
      }
      stream_ = conn;
      const std::size_t at = m.find("\"id\":");
      return {WsReply::send(R"({"result":null,"id":)" + m.substr(at + 5, m.find('}', at) - at - 5) +
                            "}")};
    }
    if (target != "/ws-fapi/v1") {
      market_ = conn; // the market data stream
      return {};
    }
    if (m.empty()) {
      return {};
    }
    const std::string id = field(m, "id");
    const std::string method = field(m, "method");
    const bool place = method == "order.place";
    if (!place && method != "order.cancel") {
      return {};
    }
    const std::string cid = field(m, place ? "newClientOrderId" : "origClientOrderId");
    if (place) {
      orders_[cid] = next_order_++;
    }
    const std::uint64_t order = orders_[cid];
    if (stream_) {
      wss_.push(*stream_, WsReply::send(update_of(cid, order, place ? "NEW" : "CANCELED")));
    }
    return {WsReply::send(R"({"id":")" + id + R"(","status":200,"result":{"orderId":)" +
                          std::to_string(order) + R"(,"symbol":"BTCUSDT","status":")" +
                          (place ? "NEW" : "CANCELED") + R"(","clientOrderId":")" + cid +
                          R"(","updateTime":)" + std::to_string(venue_ms(start_)) + "}}")};
  }

  std::string rest(const std::string& request) const {
    const std::size_t sp = request.find(' ');
    const std::string method = request.substr(0, sp);
    const std::string target = request.substr(sp + 1, request.find_first_of("? ", sp + 1) - sp - 1);
    if (target == "/fapi/v1/time") {
      return response(R"({"serverTime":)" + std::to_string(venue_ms(start_)) + "}");
    }
    if (target == "/fapi/v1/exchangeInfo") {
      return response(exchange_info_);
    }
    if (target == "/fapi/v1/positionSide/dual") {
      return response(R"({"dualSidePosition":false})");
    }
    if (target == "/fapi/v1/multiAssetsMargin") {
      return response(R"({"multiAssetsMargin":false})");
    }
    if (target == "/fapi/v2/positionRisk") {
      return response(
          R"([{"symbol":"BTCUSDT","positionAmt":"0.000","entryPrice":"0.0","markPrice":"84000","unRealizedProfit":"0","liquidationPrice":"0","leverage":"20","maxNotionalValue":"1000000","marginType":"cross","isolatedMargin":"0","isAutoAddMargin":"false","positionSide":"BOTH","notional":"0","isolatedWallet":"0","updateTime":0}])");
    }
    if (target == "/fapi/v1/listenKey") {
      return response(method == "POST" ? R"({"listenKey":"LK1"})" : "{}");
    }
    if (target == "/fapi/v1/countdownCancelAll") {
      return response(R"({"symbol":"BTCUSDT","countdownTime":"0"})");
    }
    if (target == "/fapi/v3/balance") {
      return response(
          R"([{"accountAlias":"a","asset":"USDT","balance":"10000.0","crossWalletBalance":"10000","crossUnPnl":"0","availableBalance":"10000.0","maxWithdrawAmount":"10000","marginAvailable":true,"updateTime":1700000000000}])");
    }
    return response("[]"); // open orders, trades, positions: none
  }

  std::mutex mutex_;
  Clock::time_point start_ = Clock::now();
  ScriptedWssServer wss_;
  HandlerHttpsServer https_;
  std::string exchange_info_;
  std::map<std::string, std::uint64_t> orders_;
  std::uint64_t next_order_ = 1;
  std::uint64_t update_ = 1000;
  std::optional<std::size_t> market_;
  std::optional<std::size_t> stream_;
  std::atomic<bool> streaming_{false};
  std::thread pusher_;
};

// Places an order far from the market on a quote, cancels it on the next; one command a quote
// while nothing is waiting for the venue.
struct LatencyQuoter {
  md::ClientOrderId working;
  bool has = false;
  bool canceling = false;
  std::uint64_t quotes = 0;

  static Status on_start(st::Context& ctx) {
    return ctx.subscribe_quotes(btc(), jarvis::data::Cadence::every());
  }
  void on_quote(st::Context& ctx, const md::QuoteTick& /*q*/) {
    ++quotes;
    if (!has) {
      md::Quantity qty;
      md::Price px;
      static_cast<void>(md::Quantity::parse("0.0010", qty));
      static_cast<void>(md::Price::parse("83000.0", px));
      has = jarvis::core::ok(ctx.submit(ctx.limit(btc(), md::OrderSide::Buy, qty, px), working));
    } else if (!canceling) {
      canceling = jarvis::core::ok(ctx.cancel(working));
    }
  }
  void on_order_event(st::Context& /*ctx*/, const md::OrderEvent& e) {
    if (std::holds_alternative<md::OrderCanceled>(e) ||
        std::holds_alternative<md::OrderRejected>(e) ||
        std::holds_alternative<md::OrderDenied>(e)) {
      has = false;
      canceling = false;
    }
  }
};

// The percentile `p` (0..1) of a Prometheus histogram in `metrics`: the upper bound of the bucket
// it falls in, in nanoseconds (0 without samples).
double percentile(const std::string& metrics, std::string_view name, double p) {
  std::vector<std::pair<double, double>> buckets; // (le, cumulative)
  double count = 0;
  std::istringstream lines{metrics};
  const std::string bucket = std::string{name} + "_bucket{le=\"";
  const std::string total = std::string{name} + "_count ";
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind(bucket, 0) == 0) {
      const std::size_t close = line.find('"', bucket.size());
      const std::string le = line.substr(bucket.size(), close - bucket.size());
      const double cumulative = std::stod(line.substr(line.rfind(' ') + 1));
      buckets.emplace_back(le == "+Inf" ? 1e12 : std::stod(le), cumulative);
    } else if (line.rfind(total, 0) == 0) {
      count = std::stod(line.substr(total.size()));
    }
  }
  for (const auto& [le, cumulative] : buckets) {
    if (count > 0 && cumulative >= p * count) {
      return le;
    }
  }
  return 0;
}

struct LatencyRun {
  std::string metrics;
  std::uint64_t quotes = 0;
  std::uint64_t commands = 0;
  bool ok = false;
};

LatencyRun run_latency(std::size_t quotes) {
  LatencyRun out;
  QuoteVenue venue;
  const TempDir dir;
  const std::string text = R"(
[node]
id = "lt01"
env = "live"
seed = 5
market_data_stale_ms = 0

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["BTCUSDT-PERP.BINANCE"]
streams = ["bookTicker"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"
credentials = "env:JARVIS_BENCH_UNUSED"

[[strategies]]
id = "quoter-001"
impl = "cpp:LatencyQuoter"
instruments = ["BTCUSDT-PERP.BINANCE"]

[risk]
orders_per_10s = 0
orders_per_minute = 0

[persistence]
mode = "async"
dir = ")" + dir.file("runs") +
                           R"(/{node_id}/{run_id}"

[telemetry]
prometheus = "127.0.0.1:0"
jsonl = false
)";
  node::NodeConfig config;
  std::vector<node::ConfigError> errors;
  if (!jarvis::core::ok(node::parse_config(text, "latency.toml", {}, config, errors))) {
    return out;
  }
  std::atomic<bool> stop{false};
  const live::LiveRequest request = venue.request(config, text, dir, stop);
  st::StaticStrategySet<LatencyQuoter> set{LatencyQuoter{}};
  live::LiveResult result;
  std::string error;
  Status s = Status::Ok;
  std::thread node_thread{[&] {
    node::NoHook hook;
    s = live::run_live(request, set, result, error, hook);
  }};
  const auto deadline = Clock::now() + std::chrono::seconds{30};
  while (!venue.ready() && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{500}); // reconciled and running
  venue.stream_quotes(quotes, std::chrono::microseconds{1'000});
  std::this_thread::sleep_for(std::chrono::milliseconds{static_cast<std::int64_t>(quotes) + 500});
  stop = true;
  node_thread.join();
  out.ok = jarvis::core::ok(s);
  out.metrics = result.telemetry_metrics;
  out.quotes = set.get<0>().quotes;
  out.commands = result.venue.commands;
  return out;
}

void live_latency(benchmark::State& state) {
  std::size_t quotes = 5'000;
  if (const char* n = std::getenv("JARVIS_LATENCY_QUOTES")) {
    quotes = std::strtoull(n, nullptr, 10);
  }
  LatencyRun run;
  for ([[maybe_unused]] auto _ : state) {
    run = run_latency(quotes);
  }
  if (!run.ok) {
    state.SkipWithError("the live node did not run");
    return;
  }
  for (const auto& [name, metric] :
       {std::pair{"tick_to_command", "jarvis_tick_to_command_ns"},
        std::pair{"command_to_socket", "jarvis_command_to_socket_ns"}}) {
    state.counters[std::string{name} + "_p50_ns"] = percentile(run.metrics, metric, 0.50);
    state.counters[std::string{name} + "_p90_ns"] = percentile(run.metrics, metric, 0.90);
    state.counters[std::string{name} + "_p99_ns"] = percentile(run.metrics, metric, 0.99);
  }
  state.counters["quotes"] = static_cast<double>(run.quotes);
  state.counters["commands"] = static_cast<double>(run.commands);
}

} // namespace

BENCHMARK(live_latency)
    ->Name("latency/live")
    ->Iterations(1)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);
