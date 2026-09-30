// The live node (jarvis/live/live_node.hpp) end to end against a scripted venue: startup checks
// and the listenKey over HTTPS, market data, the user data stream and the WebSocket API over WSS.
// The node reconciles, starts its strategy, places an order (with the venue's dead man's switch
// for its instrument) and sees it partly filled; on the way out it cancels the rest, waits for
// the venue's confirmation and disarms the switch. Its recording replays under the backtest
// wiring with the same outputs.

#include <atomic>
#include <chrono>
#include <filesystem>
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
#include "jarvis/node/snapshot_file.hpp"
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

// Buys once it runs; stops the node when the fill arrives (or, resumed, once reconciled). Its
// own state is the fills it heard of, so a snapshot of it is complete and a run can resume.
struct Buyer {
  Buyer() = default;
  explicit Buyer(std::atomic<bool>* s, bool when_reconciled = false)
      : stop{s}, stop_when_reconciled{when_reconciled} {}

  std::atomic<bool>* stop = nullptr;
  bool stop_when_reconciled = false;
  std::vector<std::string> log;
  std::vector<md::ReconcileOutcome> outcomes;
  std::vector<std::uint64_t> position_raw; // at each reconciliation (0: flat)
  std::uint32_t fills = 0;

  template <typename Ar> void state(Ar& ar) { ar(fills); }

  Status on_reconciled(st::Context& ctx, const md::ReconcileOutcome& o) {
    log.push_back("reconciled orders=" + std::to_string(o.orders));
    outcomes.push_back(o);
    st::PositionView p;
    position_raw.push_back(ctx.position(btc(), p) ? p.quantity.raw() : 0);
    if (stop_when_reconciled && stop != nullptr) {
      stop->store(true);
    }
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
      ++fills;
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

// The startup checks' answers: the clock, the instruments, the position mode, multi-assets, and
// the symbol's position (leverage, margin type).
std::vector<std::string> startup_answers(std::string_view position) {
  const std::string exchange_info =
      file_text(std::string{JARVIS_SOURCE_DIR} + "/tests/data/binance/exchange_info_testnet.json");
  return {
      http(R"({"serverTime":1700000002000})"), http(exchange_info),
      http(R"({"dualSidePosition":false})"), http(R"({"multiAssetsMargin":false})"),
      http(
          R"([{"symbol":"BTCUSDT","positionAmt":")" + std::string{position} +
              R"(","entryPrice":"0.0","markPrice":"84000","unRealizedProfit":"0","liquidationPrice":"0","leverage":"20","maxNotionalValue":"1000000","marginType":"cross","isolatedMargin":"0","isAutoAddMargin":"false","positionSide":"BOTH","notional":"0","isolatedWallet":"0","updateTime":0}])",
          true)};
}

// A scripted venue: the WebSocket API places orders (each fills in part at once on the user data
// stream) and cancels them; REST answers in the order given.
class MockVenue {
public:
  explicit MockVenue(std::vector<std::string> answers)
      : wss_{8, [this](std::size_t conn, const std::string& m) { return answer(conn, m); }},
        https_{std::move(answers), wss_.ca_file(), wss_.key_file()} {}
  MockVenue(const MockVenue&) = delete;
  MockVenue& operator=(const MockVenue&) = delete;

  [[nodiscard]] live::LiveRequest request(const node::NodeConfig& config, const std::string& text,
                                          const TempDir& dir, std::atomic<bool>& stop) const {
    live::LiveRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "live.toml";
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
    request.spot_rest = ""; // no key permission check against the mock
    request.credentials = node::ApiCredentials{"KEY1", "s3cret"};
    request.epoch_file = dir.file("epoch");
    request.stop = &stop;
    request.run_for = std::chrono::seconds{10};
    request.now_ms = [] { return std::int64_t{1'700'000'002'000}; };
    return request;
  }

  [[nodiscard]] std::vector<std::string> requests() { return https_.requests(); }
  [[nodiscard]] bool clean() { return wss_.error().empty() && https_.error().empty(); }

private:
  std::vector<WsReply> answer(std::size_t conn, const std::string& m) {
    if (m.empty()) {
      return {};
    }
    if (wss_.target(conn) == "/private/stream") {
      const std::size_t at = m.find("\"id\":");
      return {WsReply::send(R"({"result":null,"id":)" + m.substr(at + 5, m.find('}', at) - at - 5) +
                            "}")};
    }
    const auto to_stream = [this](const std::string& frame) {
      for (std::size_t c = 0; c < 8; ++c) {
        if (wss_.target(c) == "/private/stream") {
          wss_.push(c, WsReply::send(frame));
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
  }

  ScriptedWssServer wss_;
  ScriptedHttpsServer https_;
};

// The first run's REST answers: the startup checks, the listenKey, the snapshot (an empty
// account), then the dead man's switch for the order the strategy places, and its disarming once
// the shutdown has canceled what was left of the order.
std::vector<std::string> first_run_answers() {
  std::vector<std::string> a = startup_answers("0.000");
  for (
      std::string r :
      {http(R"({"listenKey":"LK1"})"), http("[]"), http("[]"),
       http(
           R"([{"accountAlias":"a","asset":"USDT","balance":"10000.0","crossWalletBalance":"10000","crossUnPnl":"0","availableBalance":"10000.0","maxWithdrawAmount":"10000","marginAvailable":true,"updateTime":1700000000000}])"),
       http("[]"), http("[]"), http(R"({"symbol":"BTCUSDT","countdownTime":"120000"})"),
       http(R"({"symbol":"BTCUSDT","countdownTime":"0"})")}) {
    a.push_back(std::move(r));
  }
  return a;
}

// A resumed run's REST answers: the startup checks, the listenKey, and the snapshot: no open
// orders or new trades, no balances reported, and the position the first run's fill left.
std::vector<std::string> resumed_answers() {
  std::vector<std::string> a = startup_answers("0.004");
  for (
      std::string r :
      {http(R"({"listenKey":"LK2"})"), http("[]"), http("[]"), http("[]"),
       http(
           R"([{"symbol":"BTCUSDT","positionSide":"BOTH","positionAmt":"0.004","entryPrice":"84000.0","updateTime":1700000003100}])"),
       http("[]")}) {
    a.push_back(std::move(r));
  }
  return a;
}

// The seq of the first record of a run log, and of its last input (a torn tail left out).
std::uint64_t first_seq(const std::string& dir) {
  node::EventLogReader reader;
  md::wire::RecordView record;
  REQUIRE(reader.open(dir) == Status::Ok);
  REQUIRE(reader.next(record) == Status::Ok);
  return record.header.seq;
}

std::uint64_t last_input_seq(const std::string& dir) {
  node::EventLogReader reader;
  md::wire::RecordView record;
  REQUIRE(reader.open(dir, node::EventLogReadOptions{true}) == Status::Ok);
  std::uint64_t last = 0;
  while (reader.next(record) == Status::Ok) {
    if (record.header.kind < md::wire::kFirstOutputKind) {
      last = record.header.seq;
    }
  }
  return last;
}

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
snapshot_every = 5
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
    MockVenue venue{first_run_answers()};
    const TempDir dir;
    const std::string text = config_text(dir, mode);
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "live.toml", {}, config, errors) == Status::Ok);

    std::atomic<bool> stop{false};
    live::LiveRequest request = venue.request(config, text, dir, stop);
    // Earlier runs took epochs 1 to 4: this one's ClientOrderIds carry 5, and so must the replay's
    // (the RunStart input records it).
    for (int i = 0; i < 4; ++i) {
      std::uint64_t earlier = 0;
      REQUIRE(node::next_epoch(dir.file("epoch"), earlier) == Status::Ok);
    }

    st::StaticStrategySet<Buyer> set{Buyer{&stop}};
    live::LiveResult result;
    std::string error;
    node::NoHook hook;
    const Status s = live::run_live(request, set, result, error, hook);
    INFO(error);
    REQUIRE(s == Status::Ok);
    CHECK(result.epoch == 5);
    CHECK(result.startup.passed());
    // The shutdown cancels the rest of the order; the strategy, stopped, hears only of the
    // request. The fill on the user data stream can overtake the WebSocket API's answer to the
    // order (as at the venue); the order is then accepted by its fill.
    const std::vector<std::string> acked{
        "reconciled orders=0", "start", "submitted", "accepted", "filled", "pending cancel"};
    const std::vector<std::string> overtaken{"reconciled orders=0", "start", "submitted", "filled",
                                             "pending cancel"};
    CHECK((set.get<0>().log == acked || set.get<0>().log == overtaken));
    CHECK(result.summary.left_open == 0);
    // The order, its countdown, the shutdown's cancel, the countdown's disarming.
    CHECK(result.venue.commands == 4);
    CHECK(result.venue.refused_locally == 0);
    CHECK(result.venue.snapshots == 1);
    CHECK(result.venue.decode_errors == 0);
    CHECK(result.venue.countdowns == 2);
    CHECK(result.venue.countdown_failures == 0);
    const std::vector<std::string> requests = venue.requests();
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
    // Every snapshot the persist thread wrote matches the replayed state.
    CHECK(result.summary.snapshots > 0);
    CHECK(result.summary.snapshot_failures == 0);
    CHECK(result.persist.snapshots == result.summary.snapshots);
    CHECK(report.snapshots_checked == result.summary.snapshots);
    CHECK(report.inputs == result.summary.inputs);
    CHECK(report.outputs == result.summary.outputs);
    CHECK(fresh.get<0>().log == set.get<0>().log);

    // The epoch file holds the last epoch taken.
    std::uint64_t epoch = 0;
    REQUIRE(node::read_epoch(dir.file("epoch"), epoch) == Status::Ok);
    CHECK(epoch == 5);
    CHECK(venue.clean());
  }

  TEST_CASE("a live node resumes where its earlier run stopped, also after a crash") {
    bool crash = false;
    SUBCASE("stopped") {}
    SUBCASE("crashed") { crash = true; }
    CAPTURE(crash);
    const TempDir dir;
    const std::string text = config_text(dir, "async") + "resume = true\n";
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "live.toml", {}, config, errors) == Status::Ok);
    std::string error;
    node::NoHook hook;

    // The first run (nothing to resume yet) buys, is partly filled, and cancels the rest on the
    // way out.
    std::atomic<bool> stop{false};
    st::StaticStrategySet<Buyer> set{Buyer{&stop}};
    live::LiveResult first;
    {
      MockVenue venue{first_run_answers()};
      const live::LiveRequest request = venue.request(config, text, dir, stop);
      const Status s = live::run_live(request, set, first, error, hook);
      INFO(error);
      REQUIRE(s == Status::Ok);
      CHECK(venue.clean());
    }
    CHECK(first.recovery.from.empty());
    REQUIRE(set.get<0>().fills == 1);
    REQUIRE(first.summary.snapshots > 0);
    if (crash) {
      // A crash while the last record was written: its end never reached the disk.
      const std::string segment = first.directory + "/" + node::segment_name(0);
      std::filesystem::resize_file(segment, std::filesystem::file_size(segment) - 3);
    }
    const std::uint64_t last = last_input_seq(first.directory);
    REQUIRE(last > 0);

    // The second run continues it: the state from the first run's latest snapshot and the rest
    // of its log, reconciled with the venue.
    std::atomic<bool> stop2{false};
    st::StaticStrategySet<Buyer> set2{Buyer{&stop2, true}};
    live::LiveResult second;
    {
      MockVenue venue{resumed_answers()};
      const live::LiveRequest request = venue.request(config, text, dir, stop2);
      const Status s = live::run_live(request, set2, second, error, hook);
      INFO(error);
      REQUIRE(s == Status::Ok);
      CHECK(venue.clean());
      // No order and no dead man's switch this time.
      CHECK(venue.requests().size() == resumed_answers().size());
    }
    CHECK(second.epoch == 2);
    CHECK(second.recovery.from == first.directory);
    CHECK(second.recovery.last_seq == last);
    CHECK(second.recovery.snapshot_seq > 0);
    CHECK(second.recovery.snapshot_seq <= last);
    CHECK((second.recovery.torn_bytes != 0) == crash);
    CHECK(second.venue.commands == 0);
    // The strategy continues: on_start is not called again, it still counts the fill, and the
    // position the first run left matches the venue's, so the reconciliation finds nothing.
    const Buyer& resumed = set2.get<0>();
    CHECK(resumed.log == std::vector<std::string>{"reconciled orders=0"});
    CHECK(resumed.fills == 1);
    md::Quantity filled;
    REQUIRE(md::Quantity::parse("0.004", filled) == Status::Ok);
    REQUIRE(resumed.position_raw.size() == 1);
    CHECK(resumed.position_raw[0] == filled.raw());
    REQUIRE(resumed.outcomes.size() == 1);
    CHECK(resumed.outcomes[0].diffs == 0);
    CHECK(resumed.outcomes[0].lost == 0);
    CHECK(resumed.outcomes[0].external == 0);

    // The new run's log continues the seq after the first run's last input, and its directory
    // starts with a snapshot of the state it continued from.
    CHECK(first_seq(second.directory) == last + 1);
    CHECK(std::filesystem::exists(second.directory + "/" + node::snapshot_name(last)));
    CHECK(file_text(second.directory + "/run.toml").find("resumed_from = ") != std::string::npos);
    // Its replay starts from that snapshot.
    st::StaticStrategySet<Buyer> fresh{Buyer{}};
    node::ReplayReport report;
    REQUIRE(node::replay_run(second.directory, config, fresh, node::ReplayOptions{}, report,
                             error) == Status::Ok);
    INFO((report.divergence ? report.divergence->recorded + " / " + report.divergence->replayed
                            : std::string{}));
    CHECK_FALSE(report.divergence.has_value());
    CHECK(report.start_seq == last);
    CHECK(report.inputs == second.summary.inputs);
    CHECK(report.outputs == second.summary.outputs);
    CHECK(fresh.get<0>().log == resumed.log);
    CHECK(fresh.get<0>().fills == 1);
  }
}
