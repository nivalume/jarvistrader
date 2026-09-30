// The live node (jarvis/live/live_node.hpp) end to end against a scripted venue: startup checks
// and the listenKey over HTTPS, market data, the user data stream and the WebSocket API over WSS.
// The node reconciles, starts its strategy, places an order (with the venue's dead man's switch
// for its instrument) and sees it partly filled; on the way out it cancels the rest, waits for
// the venue's confirmation and disarms the switch. Its recording replays under the backtest
// wiring with the same outputs.

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/live/live_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/epoch_store.hpp"
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
using jarvis::testsupport::ScriptedHttpsServer;
using jarvis::testsupport::ScriptedWssServer;
using jarvis::testsupport::TempDir;
using jarvis::testsupport::WsReply;

md::InstrumentId btc() {
  md::InstrumentId id;
  REQUIRE(md::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
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

std::string http(std::string_view body, bool close = false) {
  return std::string{"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"} +
         (close ? "Connection: close\r\n" : "") + "Content-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + std::string{body};
}

std::string file_text(const std::string& path) {
  std::ifstream in{path};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string fill_of(std::string_view cid) {
  return R"({"stream":"LK1","data":{"e":"ORDER_TRADE_UPDATE","E":1700000003100,"T":1700000003099,"o":{"s":"BTCUSDT","c":")" +
         std::string{cid} +
         R"(","S":"BUY","o":"LIMIT","f":"GTC","q":"0.010","p":"84000.0","ap":"84000.0","sp":"0","x":"TRADE","X":"PARTIALLY_FILLED","i":42,"l":"0.004","z":"0.004","L":"84000.0","N":"USDT","n":"0.0672","T":1700000003090,"t":7,"b":"0","a":"0","m":true,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0"}}})";
}

std::string canceled_of(std::string_view cid) {
  return R"({"stream":"LK1","data":{"e":"ORDER_TRADE_UPDATE","E":1700000004100,"T":1700000004099,"o":{"s":"BTCUSDT","c":")" +
         std::string{cid} +
         R"(","S":"BUY","o":"LIMIT","f":"GTC","q":"0.010","p":"84000.0","ap":"84000.0","sp":"0","x":"CANCELED","X":"CANCELED","i":42,"l":"0","z":"0.004","L":"0","N":"USDT","n":"0","T":1700000004090,"t":0,"b":"0","a":"0","m":false,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0"}}})";
}

// Buys once it runs; stops the node when the fill arrives.
struct Buyer {
  std::atomic<bool>* stop = nullptr;
  std::vector<std::string> log;

  Status on_reconciled(st::Context& /*ctx*/, const md::ReconcileOutcome& o) {
    log.push_back("reconciled orders=" + std::to_string(o.orders));
    return Status::Ok;
  }
  Status on_start(st::Context& ctx) {
    log.emplace_back("start");
    md::Quantity qty;
    md::Price px;
    static_cast<void>(md::Quantity::parse("0.0100", qty));
    static_cast<void>(md::Price::parse("84000.0", px));
    md::ClientOrderId id;
    return ctx.submit(ctx.limit(btc(), md::OrderSide::Buy, qty, px), id);
  }
  Status on_order_event(st::Context& /*ctx*/, const md::OrderEvent& e) {
    if (std::holds_alternative<md::OrderSubmitted>(e)) {
      log.emplace_back("submitted");
    } else if (std::holds_alternative<md::OrderAccepted>(e)) {
      log.emplace_back("accepted");
    } else if (std::holds_alternative<md::OrderPendingCancel>(e)) {
      log.emplace_back("pending cancel");
    } else if (std::holds_alternative<md::OrderFilled>(e)) {
      log.emplace_back("filled");
      if (stop != nullptr) {
        stop->store(true);
      }
    } else if (const auto* d = std::get_if<md::OrderDenied>(&e)) {
      log.push_back("denied " + std::string{d->reason.view()});
    } else if (const auto* r = std::get_if<md::OrderRejected>(&e)) {
      log.push_back("rejected " + std::string{r->reason.view()});
    } else {
      log.push_back("event " + std::to_string(e.index()));
    }
    return Status::Ok;
  }
};

std::string config_text(const TempDir& dir, std::string_view mode) {
  return R"(
[node]
id = "lv01"
env = "live"
seed = 7

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["BTCUSDT-PERP.BINANCE"]
streams = ["bookTicker"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"
credentials = "env:JARVIS_TEST_UNUSED"

[[strategies]]
id = "buyer-001"
impl = "cpp:Buyer"
instruments = ["BTCUSDT-PERP.BINANCE"]

[persistence]
mode = ")" +
         std::string{mode} +
         R"("
dir = ")" +
         dir.file("runs") +
         R"(/{node_id}/{run_id}"
)";
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("a live node syncs, trades, and its recording replays with the same outputs") {
    // Once with the default persistence, once with every command waiting for its record.
    std::string mode = "async";
    SUBCASE("async") {}
    SUBCASE("barrier") { mode = "barrier"; }
    CAPTURE(mode);
    ScriptedWssServer* self = nullptr;
    ScriptedWssServer wss{
        8, [&self](std::size_t conn, const std::string& m) -> std::vector<WsReply> {
          if (m.empty()) {
            return {};
          }
          const std::string target = self->target(conn);
          if (target == "/private/stream") {
            const std::size_t at = m.find("\"id\":");
            return {WsReply::send(R"({"result":null,"id":)" +
                                  m.substr(at + 5, m.find('}', at) - at - 5) + "}")};
          }
          const auto to_stream = [&self](const std::string& frame) {
            for (std::size_t c = 0; c < 8; ++c) {
              if (self->target(c) == "/private/stream") {
                self->push(c, WsReply::send(frame));
              }
            }
          };
          if (field(m, "method") == "order.cancel") {
            const std::string cid = field(m, "origClientOrderId");
            to_stream(canceled_of(cid));
            return {WsReply::send(
                R"({"id":")" + field(m, "id") +
                R"(","status":200,"result":{"orderId":42,"symbol":"BTCUSDT","status":"CANCELED","clientOrderId":")" +
                cid + R"(","updateTime":1700000004000}})")};
          }
          if (field(m, "method") == "order.place") {
            const std::string cid = field(m, "newClientOrderId");
            to_stream(fill_of(cid));
            return {WsReply::send(
                R"({"id":")" + field(m, "id") +
                R"(","status":200,"result":{"orderId":42,"symbol":"BTCUSDT","status":"NEW","clientOrderId":")" +
                cid + R"(","updateTime":1700000003000}})")};
          }
          return {};
        }};
    self = &wss;
    const std::string exchange_info = file_text(std::string{JARVIS_SOURCE_DIR} +
                                                "/tests/data/binance/exchange_info_testnet.json");
    ScriptedHttpsServer https{
        std::vector<std::string>{
            // Startup checks: clock, instruments, position mode, multi-assets, the symbol.
            http(R"({"serverTime":1700000002000})"), http(exchange_info),
            http(R"({"dualSidePosition":false})"), http(R"({"multiAssetsMargin":false})"),
            http(
                R"([{"symbol":"BTCUSDT","positionAmt":"0.000","entryPrice":"0.0","markPrice":"84000","unRealizedProfit":"0","liquidationPrice":"0","leverage":"20","maxNotionalValue":"1000000","marginType":"cross","isolatedMargin":"0","isAutoAddMargin":"false","positionSide":"BOTH","notional":"0","isolatedWallet":"0","updateTime":0}])",
                true),
            // The venue-io thread: the listenKey, then the snapshot (an empty account).
            http(R"({"listenKey":"LK1"})"), http("[]"), http("[]"),
            http(
                R"([{"accountAlias":"a","asset":"USDT","balance":"10000.0","crossWalletBalance":"10000","crossUnPnl":"0","availableBalance":"10000.0","maxWithdrawAmount":"10000","marginAvailable":true,"updateTime":1700000000000}])"),
            http("[]"), http("[]"),
            // The dead man's switch for the order the strategy places, and its disarming once
            // the shutdown has canceled what was left of the order.
            http(R"({"symbol":"BTCUSDT","countdownTime":"120000"})"),
            http(R"({"symbol":"BTCUSDT","countdownTime":"0"})")},
        wss.ca_file(), wss.key_file()};

    const TempDir dir;
    const std::string text = config_text(dir, mode);
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "live.toml", {}, config, errors) == Status::Ok);

    std::atomic<bool> stop{false};
    live::LiveRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "live.toml";
    live::FeedEndpoints market;
    market.streams = wss.url("");
    market.ws_api = wss.url("/ws-fapi/v1");
    market.tls.ca_file = wss.ca_file();
    request.market = market;
    live::VenueEndpoints venue;
    venue.rest = https.url();
    venue.ws_api = wss.url("/ws-fapi/v1");
    venue.user_stream = wss.url("/private/stream");
    venue.tls.ca_file = wss.ca_file();
    request.venue = venue;
    request.spot_rest = ""; // no key permission check against the mock
    request.credentials = node::ApiCredentials{"KEY1", "s3cret"};
    request.epoch_file = dir.file("epoch");
    request.stop = &stop;
    request.run_for = std::chrono::seconds{10};
    request.now_ms = [] { return std::int64_t{1'700'000'002'000}; };

    st::StaticStrategySet<Buyer> set{Buyer{&stop, {}}};
    live::LiveResult result;
    std::string error;
    node::NoHook hook;
    const Status s = live::run_live(request, set, result, error, hook);
    INFO(error);
    REQUIRE(s == Status::Ok);
    CHECK(result.epoch == 1);
    CHECK(result.startup.passed());
    // The shutdown cancels the rest of the order; the strategy, stopped, hears only of the
    // request.
    CHECK(set.get<0>().log == std::vector<std::string>{"reconciled orders=0", "start", "submitted",
                                                       "accepted", "filled", "pending cancel"});
    CHECK(result.summary.left_open == 0);
    // The order, its countdown, the shutdown's cancel, the countdown's disarming.
    CHECK(result.venue.commands == 4);
    CHECK(result.venue.refused_locally == 0);
    CHECK(result.venue.snapshots == 1);
    CHECK(result.venue.decode_errors == 0);
    CHECK(result.venue.countdowns == 2);
    CHECK(result.venue.countdown_failures == 0);
    const std::vector<std::string> requests = https.requests();
    REQUIRE(requests.size() >= 2);
    const std::string& arm = requests[requests.size() - 2];
    CHECK(arm.starts_with("POST /fapi/v1/countdownCancelAll"));
    CHECK(arm.find("symbol=BTCUSDT&countdownTime=120000") != std::string::npos);
    CHECK(requests.back().find("symbol=BTCUSDT&countdownTime=0&") != std::string::npos);
    CHECK(result.summary.state == md::NodeState::Stopped);
    REQUIRE_FALSE(result.directory.empty());
    // Every input and output went through the persist thread and is durable.
    CHECK(result.persist.records == result.summary.inputs + result.summary.outputs);
    CHECK(result.persist.durable == result.persist.position);
    CHECK(result.persist.stalls == 0);
    CHECK(result.venue.unsent_at_stop == 0);
    if (mode == "barrier") {
      CHECK(result.persist.syncs >= 1);
      CHECK(result.venue.barrier_waits <= result.venue.commands);
    } else {
      CHECK(result.venue.barrier_waits == 0);
    }

    // The environment equivalence: the recorded live session under the backtest wiring.
    st::StaticStrategySet<Buyer> fresh{Buyer{}};
    node::ReplayReport report;
    REQUIRE(node::replay_run(result.directory, config, fresh, node::ReplayOptions{}, report,
                             error) == Status::Ok);
    INFO((report.divergence ? report.divergence->recorded + " / " + report.divergence->replayed
                            : std::string{}));
    CHECK_FALSE(report.divergence.has_value());
    CHECK(report.inputs == result.summary.inputs);
    CHECK(report.outputs == result.summary.outputs);
    CHECK(fresh.get<0>().log == set.get<0>().log);

    // The next start takes the next epoch.
    std::uint64_t epoch = 0;
    REQUIRE(node::read_epoch(dir.file("epoch"), epoch) == Status::Ok);
    CHECK(epoch == 1);
    CHECK(wss.error().empty());
    CHECK(https.error().empty());
  }
}
