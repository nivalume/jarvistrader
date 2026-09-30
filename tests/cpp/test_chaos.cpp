// Chaos (docs/architecture.md section 17.2): the live node against a venue whose user data
// stream loses, repeats and reorders messages and drops its connection, the network faults the
// Reconciliation spec models. The venue keeps its own book (orders, trades, the position) and
// answers the REST snapshot from it by path, so the node's reconciliation after each reconnect
// has to recover whatever the stream lost. After a calm end, one more reconnect and a clean
// shutdown, the node must agree with the venue: every trade counted once, the same position,
// nothing left open; and its recording must replay with the same outputs.
//
// Each seed is a different run; JARVIS_CHAOS_SEEDS (a count) runs more of them (nightly).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/live/live_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/replay.hpp"
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
constexpr std::int64_t kLotsPerOrder = 3; // of 0.001 BTC

md::InstrumentId btc() {
  md::InstrumentId id;
  REQUIRE(md::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
}

std::string file_text(const std::string& path) {
  std::ifstream in{path};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A JSON string field or a query/form parameter, by name.
std::string field(const std::string& json, std::string_view key) {
  const std::string open = "\"" + std::string{key} + "\":\"";
  const std::size_t at = json.find(open);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + open.size();
  return json.substr(start, json.find('"', start) - start);
}

std::string param(const std::string& request, std::string_view key) {
  const std::string k = std::string{key} + "=";
  for (std::size_t at = request.find(k); at != std::string::npos; at = request.find(k, at + 1)) {
    if (at > 0 && (request[at - 1] == '?' || request[at - 1] == '&')) {
      const std::size_t start = at + k.size();
      const std::size_t end = request.find_first_of("& \r\n", start);
      return request.substr(start, end - start);
    }
  }
  return {};
}

std::string lots_text(std::int64_t lots) {
  const std::int64_t a = lots < 0 ? -lots : lots;
  std::string frac = std::to_string(a % 1000);
  frac.insert(0, 3 - frac.size(), '0');
  return (lots < 0 ? "-" : "") + std::to_string(a / 1000) + "." + frac;
}

std::string response(std::string_view body, int status = 200) {
  return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Bad Request") +
         "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

// The venue's clock, which the node's local clock follows (so T_s and event times agree).
struct VenueClock {
  Clock::time_point start = Clock::now();
  [[nodiscard]] std::int64_t now_ms() const {
    return kBaseMs +
           std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  }
};

struct ChaosStats {
  std::size_t dropped = 0;
  std::size_t duplicated = 0;
  std::size_t delayed = 0;
  std::size_t lost_offline = 0; // emitted while no stream was connected
  std::size_t disconnects = 0;
  std::size_t fills = 0;
};

// A venue with its own book. The WebSocket API places and cancels orders; a ticker fills open
// orders one lot at a time and drops the user stream now and then; REST answers the startup
// checks, the listenKey, the dead man's switch and the snapshot from the book.
class ChaosVenue {
public:
  explicit ChaosVenue(std::uint64_t seed)
      : rng_{seed},
        wss_{64, [this](std::size_t conn, const std::string& m) { return ws(conn, m); }},
        https_{[this](const std::string& r) { return rest(r); }, wss_.ca_file(), wss_.key_file()},
        exchange_info_{file_text(std::string{JARVIS_SOURCE_DIR} +
                                 "/tests/data/binance/exchange_info_testnet.json")},
        ticker_{[this] { tick_loop(); }} {}
  ~ChaosVenue() {
    done_ = true;
    ticker_.join();
  }
  ChaosVenue(const ChaosVenue&) = delete;
  ChaosVenue& operator=(const ChaosVenue&) = delete;

  [[nodiscard]] live::LiveRequest request(const node::NodeConfig& config, const std::string& text,
                                          const TempDir& dir, std::atomic<bool>& stop) const {
    live::LiveRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "chaos.toml";
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
    request.run_for = std::chrono::seconds{60};
    request.now_ms = [clock = &clock_] { return clock->now_ms(); };
    return request;
  }

  // No more faults or fills: every message gets through; open orders only wait.
  void calm() {
    const std::lock_guard lock{mutex_};
    calm_ = true;
    if (held_) {
      send(*held_);
      held_.reset();
    }
  }
  // Drops the user stream's connection now.
  void disconnect() {
    const std::lock_guard lock{mutex_};
    drop_stream();
  }

  [[nodiscard]] ChaosStats stats() {
    const std::lock_guard lock{mutex_};
    return stats_;
  }
  [[nodiscard]] std::set<std::string> trade_ids() {
    const std::lock_guard lock{mutex_};
    std::set<std::string> out;
    for (const Trade& t : trades_) {
      out.insert(std::to_string(t.id));
    }
    return out;
  }
  [[nodiscard]] std::int64_t position_lots() {
    const std::lock_guard lock{mutex_};
    return position_;
  }
  [[nodiscard]] std::size_t open_orders() {
    const std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(std::count_if(orders_.begin(), orders_.end(),
                                                  [](const auto& o) { return o.second.open(); }));
  }
  [[nodiscard]] std::string errors() { return wss_.error() + https_.error(); }

private:
  struct Order {
    std::string cid;
    std::uint64_t id = 0;
    bool buy = true;
    std::int64_t lots = kLotsPerOrder;
    std::int64_t filled = 0;
    std::string status = "NEW";
    std::int64_t time = 0;
    std::int64_t update = 0;
    [[nodiscard]] bool open() const { return status == "NEW" || status == "PARTIALLY_FILLED"; }
  };
  struct Trade {
    std::uint64_t id = 0;
    std::uint64_t order = 0;
    bool buy = true;
    std::int64_t time = 0;
  };

  // ---- the user data stream, through the faults ------------------------------------------

  void send(const std::string& frame) {
    if (!stream_) {
      ++stats_.lost_offline;
      return;
    }
    wss_.push(*stream_, WsReply::send(frame));
  }

  void drop_stream() {
    if (stream_) {
      wss_.push(*stream_, WsReply::drop());
      stream_.reset();
      ++stats_.disconnects;
    }
    held_.reset(); // in flight when the connection went: lost
  }

  void emit(const std::string& frame) {
    if (calm_) {
      send(frame);
      return;
    }
    const double r = std::uniform_real_distribution<double>{0.0, 1.0}(rng_);
    if (r < 0.08) {
      ++stats_.dropped;
      next_drop_ = std::min(next_drop_, clock_.now_ms() + 150); // soon reconciled
    } else if (r < 0.16) {
      ++stats_.duplicated;
      send(frame);
      send(frame);
    } else if (r < 0.28 && !held_) {
      ++stats_.delayed;
      held_ = frame; // overtaken by the next one
      return;
    } else {
      send(frame);
    }
    if (held_) {
      send(*held_);
      held_.reset();
    }
  }

  static std::string update_of(const Order& o, std::string_view exec, std::int64_t last,
                               std::uint64_t trade, std::int64_t t) {
    return R"({"stream":"LK1","data":{"e":"ORDER_TRADE_UPDATE","E":)" + std::to_string(t) +
           R"(,"T":)" + std::to_string(t) + R"(,"o":{"s":"BTCUSDT","c":")" + o.cid + R"(","S":")" +
           (o.buy ? "BUY" : "SELL") + R"(","o":"LIMIT","f":"GTC","q":")" + lots_text(o.lots) +
           R"(","p":"84000.0","ap":"84000.0","sp":"0","x":")" + std::string{exec} + R"(","X":")" +
           o.status + R"(","i":)" + std::to_string(o.id) + R"(,"l":")" + lots_text(last) +
           R"(","z":")" + lots_text(o.filled) + R"(","L":")" + (last > 0 ? "84000.0" : "0") +
           R"(","N":"USDT","n":")" + (last > 0 ? "0.0168" : "0") + R"(","T":)" + std::to_string(t) +
           R"(,"t":)" + std::to_string(trade) +
           R"(,"b":"0","a":"0","m":true,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0"}}})";
  }

  // ---- the ticker: fills and dropped connections ------------------------------------------

  void tick_loop() {
    while (!done_) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
      const std::lock_guard lock{mutex_};
      if (calm_) {
        continue;
      }
      const std::int64_t now = clock_.now_ms();
      if (now >= next_drop_) {
        drop_stream();
        next_drop_ = now + std::uniform_int_distribution<std::int64_t>{900, 1300}(rng_);
      }
      if (now < quiet_until_ || std::uniform_int_distribution<int>{0, 9}(rng_) > 2) {
        continue;
      }
      std::vector<Order*> open;
      for (auto& [cid, o] : orders_) {
        if (o.open()) {
          open.push_back(&o);
        }
      }
      if (open.empty()) {
        continue;
      }
      Order& o = *open[std::uniform_int_distribution<std::size_t>{0, open.size() - 1}(rng_)];
      ++o.filled;
      o.status = o.filled == o.lots ? "FILLED" : "PARTIALLY_FILLED";
      o.update = now;
      const std::uint64_t id = next_trade_++;
      trades_.push_back(Trade{id, o.id, o.buy, now});
      position_ += o.buy ? 1 : -1;
      ++stats_.fills;
      emit(update_of(o, "TRADE", 1, id, now));
    }
  }

  // ---- the WebSocket API and the user stream's subscription --------------------------------

  std::vector<WsReply> ws(std::size_t conn, const std::string& m) {
    if (m.empty()) {
      return {};
    }
    const std::string target = wss_.target(conn);
    const std::lock_guard lock{mutex_};
    if (target == "/private/stream") {
      const std::size_t at = m.find("\"id\":");
      if (m.find("\"SUBSCRIBE\"") != std::string::npos) {
        stream_ = conn;
      }
      return {WsReply::send(R"({"result":null,"id":)" + m.substr(at + 5, m.find('}', at) - at - 5) +
                            "}")};
    }
    if (target != "/ws-fapi/v1") {
      return {};
    }
    const std::string id = field(m, "id");
    const std::int64_t now = clock_.now_ms();
    if (field(m, "method") == "order.place") {
      Order o;
      o.cid = field(m, "newClientOrderId");
      o.id = next_order_++;
      o.buy = field(m, "side") == "BUY";
      o.time = now;
      o.update = now;
      orders_[o.cid] = o;
      emit(update_of(o, "NEW", 0, 0, now));
      return {WsReply::send(R"({"id":")" + id + R"(","status":200,"result":{"orderId":)" +
                            std::to_string(o.id) +
                            R"(,"symbol":"BTCUSDT","status":"NEW","clientOrderId":")" + o.cid +
                            R"(","updateTime":)" + std::to_string(now) + "}}")};
    }
    if (field(m, "method") == "order.cancel") {
      const auto it = orders_.find(field(m, "origClientOrderId"));
      if (it == orders_.end() || !it->second.open()) {
        return {
            WsReply::send(R"({"id":")" + id +
                          R"(","status":400,"error":{"code":-2011,"msg":"Unknown order sent."}})")};
      }
      Order& o = it->second;
      o.status = "CANCELED";
      o.update = now;
      emit(update_of(o, "CANCELED", 0, 0, now));
      return {WsReply::send(R"({"id":")" + id + R"(","status":200,"result":{"orderId":)" +
                            std::to_string(o.id) +
                            R"(,"symbol":"BTCUSDT","status":"CANCELED","clientOrderId":")" + o.cid +
                            R"(","updateTime":)" + std::to_string(now) + "}}")};
    }
    return {};
  }

  // ---- REST ----------------------------------------------------------------------------------

  [[nodiscard]] static std::string order_json(const Order& o) {
    return R"({"avgPrice":"84000.0","clientOrderId":")" + o.cid +
           R"(","cumQuote":"0","executedQty":")" + lots_text(o.filled) + R"(","orderId":)" +
           std::to_string(o.id) + R"(,"origQty":")" + lots_text(o.lots) +
           R"(","origType":"LIMIT","price":"84000.0","reduceOnly":false,"side":")" +
           (o.buy ? "BUY" : "SELL") + R"(","positionSide":"BOTH","status":")" + o.status +
           R"(","stopPrice":"0","closePosition":false,"symbol":"BTCUSDT","time":)" +
           std::to_string(o.time) + R"(,"timeInForce":"GTC","type":"LIMIT","updateTime":)" +
           std::to_string(o.update) +
           R"(,"workingType":"CONTRACT_PRICE","priceProtect":false,"priceMatch":"NONE","selfTradePreventionMode":"NONE","goodTillDate":0})";
  }

  [[nodiscard]] std::string trades_json(const std::string& request) const {
    const std::string from = param(request, "fromId");
    const std::string since = param(request, "startTime");
    std::string out;
    for (const Trade& t : trades_) {
      if ((!from.empty() && t.id < std::stoull(from)) ||
          (from.empty() && !since.empty() && t.time < std::stoll(since))) {
        continue;
      }
      out += (out.empty() ? "" : ",") + std::string{R"({"buyer":)"} + (t.buy ? "true" : "false") +
             R"(,"commission":"0.0168","commissionAsset":"USDT","id":)" + std::to_string(t.id) +
             R"(,"maker":true,"orderId":)" + std::to_string(t.order) +
             R"(,"price":"84000.0","qty":"0.001","quoteQty":"84.0","realizedPnl":"0","side":")" +
             (t.buy ? "BUY" : "SELL") + R"(","positionSide":"BOTH","symbol":"BTCUSDT","time":)" +
             std::to_string(t.time) + "}";
    }
    return "[" + out + "]";
  }

  std::string rest(const std::string& request) {
    const std::size_t sp = request.find(' ');
    const std::string method = request.substr(0, sp);
    const std::string target = request.substr(sp + 1, request.find_first_of("? ", sp + 1) - sp - 1);
    const std::lock_guard lock{mutex_};
    const std::int64_t now = clock_.now_ms();
    if (target == "/fapi/v1/time") {
      return response(R"({"serverTime":)" + std::to_string(now) + "}");
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
    if (target == "/fapi/v2/positionRisk") { // the startup check: leverage and margin type
      return response(
          R"([{"symbol":"BTCUSDT","positionAmt":")" + lots_text(position_) +
          R"(","entryPrice":"0.0","markPrice":"84000","unRealizedProfit":"0","liquidationPrice":"0","leverage":"20","maxNotionalValue":"1000000","marginType":"cross","isolatedMargin":"0","isAutoAddMargin":"false","positionSide":"BOTH","notional":"0","isolatedWallet":"0","updateTime":0}])");
    }
    if (target == "/fapi/v1/listenKey") {
      return response(method == "POST" ? R"({"listenKey":"LK1"})" : "{}");
    }
    if (target == "/fapi/v1/countdownCancelAll") {
      return response(R"({"symbol":"BTCUSDT","countdownTime":")" + param(request, "countdownTime") +
                      "\"}");
    }
    // The snapshot's reads: no fills until it is done, so no trade shares a millisecond with
    // T_s after the trades were read (a real venue's clock has the same edge).
    quiet_until_ = now + 30;
    if (target == "/fapi/v1/openOrders") {
      std::string out;
      for (const auto& [cid, o] : orders_) {
        if (o.open()) {
          out += (out.empty() ? "" : ",") + order_json(o);
        }
      }
      return response("[" + out + "]");
    }
    if (target == "/fapi/v1/order" && method == "GET") {
      const auto it = orders_.find(param(request, "origClientOrderId"));
      return it == orders_.end() ? response(R"({"code":-2013,"msg":"Order does not exist."})", 400)
                                 : response(order_json(it->second));
    }
    if (target == "/fapi/v1/userTrades") {
      return response(trades_json(request));
    }
    if (target == "/fapi/v3/balance") {
      return response(
          R"([{"accountAlias":"a","asset":"USDT","balance":"10000.0","crossWalletBalance":"10000","crossUnPnl":"0","availableBalance":"10000.0","maxWithdrawAmount":"10000","marginAvailable":true,"updateTime":1700000000000}])");
    }
    if (target == "/fapi/v3/positionRisk") {
      return response(
          R"([{"symbol":"BTCUSDT","positionSide":"BOTH","positionAmt":")" + lots_text(position_) +
          R"(","entryPrice":"84000.0","breakEvenPrice":"84000","markPrice":"84000.0","unRealizedProfit":"0","liquidationPrice":"0","isolatedMargin":"0","notional":"0","marginAsset":"USDT","isolatedWallet":"0","initialMargin":"0","maintMargin":"0","positionInitialMargin":"0","openOrderInitialMargin":"0","adl":1,"bidNotional":"0","askNotional":"0","updateTime":)" +
          std::to_string(now) + "}]");
    }
    return response(R"({"code":-1000,"msg":"not in the chaos venue"})", 400);
  }

  std::mutex mutex_;
  std::mt19937_64 rng_;
  VenueClock clock_;
  ScriptedWssServer wss_;
  HandlerHttpsServer https_;
  std::string exchange_info_;
  std::map<std::string, Order> orders_;
  std::vector<Trade> trades_;
  std::int64_t position_ = 0; // lots, signed
  std::uint64_t next_order_ = 100;
  std::uint64_t next_trade_ = 1000;
  std::optional<std::size_t> stream_; // the subscribed user stream connection
  std::optional<std::string> held_;   // a frame waiting to be overtaken
  std::int64_t next_drop_ = kBaseMs + 1'500;
  std::int64_t quiet_until_ = 0;
  bool calm_ = false;
  ChaosStats stats_;
  std::atomic<bool> done_{false};
  std::thread ticker_;
};

// Every 50 ms: cancels the oldest working order when three are open, else places another one of
// three lots, buying and selling in turn. Counts the trades it hears of (a trade counted twice
// is a duplicate) and the reconciliations; keeps its position at on_stop.
struct ChaosTrader {
  ChaosTrader() = default;
  explicit ChaosTrader(std::atomic<int>* r) : reconciled{r} {}

  std::atomic<int>* reconciled = nullptr;
  std::set<std::string> trades;
  std::size_t duplicate_fills = 0;
  bool buy = true;
  std::int64_t final_lots = 0;
  std::vector<std::string> open_at_stop; // what the node had open when it began to stop

  static Status on_start(st::Context& ctx) {
    return ctx.set_timer(1, jarvis::core::UnixNanos{ctx.now().value() + 50'000'000},
                         jarvis::core::DurationNanos{50'000'000});
  }
  Status on_timer(st::Context& ctx, jarvis::core::TimerKey /*key*/,
                  jarvis::core::UnixNanos /*deadline*/) {
    std::array<st::OrderView, 8> open{};
    const std::size_t n = ctx.open_orders(open);
    if (n >= 3) {
      for (std::size_t i = 0; i < n; ++i) {
        if (open[i].status != md::OrderStatus::PendingCancel) {
          static_cast<void>(ctx.cancel(open[i].client_order_id));
          break;
        }
      }
      return Status::Ok;
    }
    md::Quantity qty;
    md::Price px;
    static_cast<void>(md::Quantity::parse("0.0030", qty)); // the instrument's precision
    static_cast<void>(md::Price::parse("84000.0", px));
    md::ClientOrderId id;
    const Status s =
        ctx.submit(ctx.limit(btc(), buy ? md::OrderSide::Buy : md::OrderSide::Sell, qty, px), id);
    buy = !buy;
    return s;
  }
  void on_order_event(st::Context& /*ctx*/, const md::OrderEvent& e) {
    if (const auto* f = std::get_if<md::OrderFilled>(&e)) {
      if (!trades.insert(std::string{f->trade_id.view()}).second) {
        ++duplicate_fills;
      }
    }
  }
  Status on_reconciled(st::Context& /*ctx*/, const md::ReconcileOutcome& /*o*/) const {
    if (reconciled != nullptr) {
      ++*reconciled;
    }
    return Status::Ok;
  }
  void on_stop(st::Context& ctx) {
    std::array<st::OrderView, 16> open{};
    const std::size_t n = ctx.open_orders(open);
    for (std::size_t i = 0; i < n; ++i) {
      open_at_stop.push_back(std::string{open[i].client_order_id.view()} + " " +
                             std::string{md::to_string(open[i].status)} + " filled " +
                             std::to_string(open[i].filled.raw()));
    }
    st::PositionView p;
    if (ctx.position(btc(), p)) {
      const auto lots = static_cast<std::int64_t>(p.quantity.raw() / 1'000'000);
      final_lots = p.side == md::PositionSide::Short ? -lots : lots;
    }
  }
};

std::string config_text(const TempDir& dir) {
  return R"(
[node]
id = "cx01"
env = "live"
seed = 3
market_data_stale_ms = 0

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["BTCUSDT-PERP.BINANCE"]
streams = ["bookTicker"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"
credentials = "env:JARVIS_TEST_UNUSED"

[[strategies]]
id = "chaos-001"
impl = "cpp:ChaosTrader"
instruments = ["BTCUSDT-PERP.BINANCE"]

[persistence]
mode = "async"
dir = ")" +
         dir.file("runs") + R"(/{node_id}/{run_id}"
)";
}

void wait_for(const std::function<bool()>& done, std::chrono::milliseconds limit) {
  const auto end = Clock::now() + limit;
  while (!done() && Clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
}

ChaosStats run_chaos(std::uint64_t seed) {
  CAPTURE(seed);
  ChaosVenue venue{seed};
  const TempDir dir;
  const std::string text = config_text(dir);
  node::NodeConfig config;
  std::vector<node::ConfigError> errors;
  REQUIRE(node::parse_config(text, "chaos.toml", {}, config, errors) == Status::Ok);

  std::atomic<bool> stop{false};
  std::atomic<int> reconciled{0};
  const live::LiveRequest request = venue.request(config, text, dir, stop);
  st::StaticStrategySet<ChaosTrader> set{ChaosTrader{&reconciled}};
  live::LiveResult result;
  std::string error;
  Status s = Status::Ok;
  std::thread node_thread{[&] {
    node::NoHook hook;
    s = live::run_live(request, set, result, error, hook);
  }};

  // Faults until at least three disconnects and ten fills (slow builds take longer), then calm;
  // one more reconnect so that the last reconciliation sees everything the faults hid; then the
  // shutdown cancels what is open.
  wait_for(
      [&] {
        const ChaosStats c = venue.stats();
        return c.disconnects >= 3 && c.fills >= 10;
      },
      std::chrono::milliseconds{90'000});
  venue.calm();
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  const int before = reconciled.load();
  venue.disconnect();
  wait_for([&] { return reconciled.load() > before; }, std::chrono::milliseconds{10'000});
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  stop = true;
  node_thread.join();

  INFO(error);
  REQUIRE(s == Status::Ok);
  CHECK(venue.errors().empty());
  const ChaosStats chaos = venue.stats();
  MESSAGE("seed " << seed << ": fills " << chaos.fills << ", dropped " << chaos.dropped
                  << ", duplicated " << chaos.duplicated << ", delayed " << chaos.delayed
                  << ", disconnects " << chaos.disconnects << ", lost offline "
                  << chaos.lost_offline << ", reconciliations " << reconciled.load());
  // The faults happened (duplicates and reordering are checked over all seeds).
  CHECK(chaos.disconnects >= 3);
  CHECK(chaos.dropped + chaos.lost_offline > 0);
  CHECK(chaos.fills >= 10);
  CHECK(reconciled.load() >= 2); // the first and the last at least

  // The node agrees with the venue.
  const ChaosTrader& trader = set.get<0>();
  CHECK(result.summary.state == md::NodeState::Stopped);
  CHECK(trader.duplicate_fills == 0);
  CHECK(trader.trades == venue.trade_ids());
  CHECK(trader.final_lots == venue.position_lots());
  CHECK(result.summary.left_open == 0);
  if (result.summary.left_open != 0) {
    for (const std::string& o : trader.open_at_stop) {
      MESSAGE("open when the node began to stop: " << o);
    }
  }
  CHECK(venue.open_orders() == 0);
  CHECK(result.venue.decode_errors == 0);

  // JARVIS_CHAOS_KEEP=<dir>: a failed seed's run directory is copied there to be looked at.
  if (const char* keep = std::getenv("JARVIS_CHAOS_KEEP");
      keep != nullptr && (trader.trades != venue.trade_ids() || result.summary.left_open != 0 ||
                          trader.final_lots != venue.position_lots())) {
    const std::filesystem::path to = std::filesystem::path{keep} / ("seed-" + std::to_string(seed));
    std::filesystem::create_directories(to);
    std::filesystem::copy(result.directory, to,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    MESSAGE("kept the run in " << to.string());
  }

  // And its recording replays with the same outputs.
  st::StaticStrategySet<ChaosTrader> fresh{ChaosTrader{}};
  node::ReplayReport report;
  node::ReplayOptions replay;
  replay.dump_state = std::getenv("JARVIS_CHAOS_KEEP") != nullptr;
  REQUIRE(node::replay_run(result.directory, config, fresh, replay, report, error) == Status::Ok);
  if (replay.dump_state) {
    std::ofstream{std::filesystem::path{std::getenv("JARVIS_CHAOS_KEEP")} /
                  ("state-" + std::to_string(seed) + ".txt")}
        << report.state;
  }
  INFO((report.divergence ? report.divergence->recorded + " / " + report.divergence->replayed
                          : std::string{}));
  CHECK_FALSE(report.divergence.has_value());
  CHECK(fresh.get<0>().trades == trader.trades);
  return chaos;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("chaos: the live node recovers what a faulty user stream loses, repeats or reorders") {
    std::uint64_t seeds = 2;
    if (const char* n = std::getenv("JARVIS_CHAOS_SEEDS")) {
      seeds = std::strtoull(n, nullptr, 10);
    }
    ChaosStats total;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
      const ChaosStats c = run_chaos(seed);
      total.duplicated += c.duplicated;
      total.delayed += c.delayed;
    }
    CHECK(total.duplicated > 0);
    CHECK(total.delayed > 0);
  }
}
