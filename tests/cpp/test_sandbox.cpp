// The sandbox node end to end against a scripted venue (docs/architecture.md sections 4.1 and
// 4.6): the market feed thread syncs a book from diffs and a WebSocket API snapshot, a strategy
// trades on the simulated exchange in real time, and the recorded run replays under the
// backtest wiring with the same outputs, byte for byte (the environment equivalence test).

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/redecode.hpp"
#include "jarvis/live/sandbox_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "support/ws_test.hpp"

namespace {

namespace live = jarvis::live;
namespace md = jarvis::model;
namespace st = jarvis::strategy;
namespace node = jarvis::node;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
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

std::string diff(std::uint64_t first, std::uint64_t last, std::uint64_t prev, std::string_view bids,
                 std::string_view asks) {
  return R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1700000000100,"T":1700000000099,"s":"BTCUSDT","ps":"BTCUSDT","U":)" +
         std::to_string(first) + R"(,"u":)" + std::to_string(last) + R"(,"pu":)" +
         std::to_string(prev) + R"(,"b":[)" + std::string{bids} + R"(],"a":[)" + std::string{asks} +
         "]}}";
}

std::string book_ticker(std::uint64_t u, std::string_view bid, std::string_view ask) {
  return R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":)" + std::to_string(u) +
         R"(,"s":"BTCUSDT","ps":"BTCUSDT","b":")" + std::string{bid} + R"(","B":"1.5000","a":")" +
         std::string{ask} + R"(","A":"2.0000","T":1700000000200,"E":1700000000201}})";
}

std::string agg_trade(std::uint64_t id, std::string_view px) {
  return R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1700000000300,"a":)" +
         std::to_string(id) + R"(,"s":"BTCUSDT","p":")" + std::string{px} +
         R"(","q":"0.0300","f":1,"l":1,"T":1700000000299,"m":false}})";
}

std::string snapshot_answer(const std::string& id) {
  return R"({"id":")" + id +
         R"(","status":200,"result":{"lastUpdateId":100,"E":1700000000050,"T":1700000000049,"bids":[["84550.0","1.0000"],["84549.9","2.0000"]],"asks":[["84550.1","1.5000"],["84550.2","3.0000"]]},"rateLimits":[{"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","intervalNum":1,"limit":2400,"count":20}]})";
}

// Buys at market on the first quote; once filled, rests a sell far away and cancels it when a
// timer fires. Counts what it saw so the test knows the scenario ran.
struct Tapper {
  int quotes = 0;
  int trades = 0;
  int books = 0;
  int fills = 0;
  int cancels = 0;
  bool bought = false;
  md::ClientOrderId resting;

  static Status on_start(st::Context& ctx) {
    Status s = ctx.subscribe_quotes(btc());
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_trades(btc());
    }
    if (jarvis::core::ok(s)) {
      s = ctx.subscribe_book(btc());
    }
    return s;
  }
  Status on_quote(st::Context& ctx, const md::QuoteTick& /*q*/) {
    ++quotes;
    if (bought) {
      return Status::Ok;
    }
    bought = true;
    md::Quantity qty;
    static_cast<void>(md::Quantity::parse("0.0010", qty));
    md::ClientOrderId id;
    return ctx.submit(ctx.market(btc(), md::OrderSide::Buy, qty), id);
  }
  void on_trade(st::Context& /*ctx*/, const md::TradeTick& /*t*/) { ++trades; }
  void on_book(st::Context& /*ctx*/, const jarvis::data::BookView& /*b*/) { ++books; }
  Status on_order_event(st::Context& ctx, const md::OrderEvent& e) {
    if (std::holds_alternative<md::OrderCanceled>(e)) {
      ++cancels;
    }
    if (!std::holds_alternative<md::OrderFilled>(e) || fills++ > 0) {
      return Status::Ok;
    }
    md::Quantity qty;
    md::Price px;
    static_cast<void>(md::Quantity::parse("0.0010", qty));
    static_cast<void>(md::Price::parse("90000.0", px));
    Status s = ctx.submit(ctx.limit(btc(), md::OrderSide::Sell, qty, px), resting);
    if (jarvis::core::ok(s)) {
      s = ctx.set_timer(1, UnixNanos{ctx.now().value() + 20'000'000});
    }
    return s;
  }
  void on_timer(st::Context& ctx, jarvis::core::TimerKey /*key*/, UnixNanos /*deadline*/) const {
    static_cast<void>(ctx.cancel(resting));
  }
};

std::string config_text(const TempDir& dir) {
  return R"(
[node]
id = "sbx01"
env = "sandbox"
seed = 7

[[data.streams]]
venue = "BINANCE_USDM"
instruments = ["BTCUSDT-PERP.BINANCE"]
streams = ["aggTrade", "bookTicker", "depth@100ms"]

[[venues]]
id = "BINANCE_USDM"
kind = "binance_usdm"
exchange_info = ")" +
         std::string{JARVIS_SOURCE_DIR} + R"(/tests/data/binance/exchange_info_testnet.json"

[venues.sim]
fill_model = "top_of_book"
latency = { feed_ns = 0, out_ns = 5000000, in_ns = 2000000, jitter_ns = 1000000 }
fee = { schedule = "binance_usdm_vip0" }
balances = ["10000 USDT"]

[[strategies]]
id = "tap-001"
impl = "cpp:Tapper"
instruments = ["BTCUSDT-PERP.BINANCE"]

[persistence]
dir = ")" +
         dir.file("runs") +
         R"(/{node_id}/{run_id}"
)";
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("a sandbox session replays under the backtest wiring with the same outputs") {
    ScriptedWssServer* self = nullptr;
    ScriptedWssServer server{
        8, [&self](std::size_t conn, const std::string& m) {
          const std::string target = self->target(conn);
          std::vector<WsReply> out;
          if (m.empty() && target.starts_with("/public/stream")) {
            // Buffered until the snapshot: this diff spans its update id.
            out.push_back(WsReply::send(diff(95, 105, 90, R"(["84550.0","1.2000"])", "")));
            out.push_back(WsReply::send(book_ticker(1000, "84550.0", "84550.1")));
          } else if (m.empty() && target.starts_with("/market/stream")) {
            out.push_back(WsReply::send(agg_trade(1, "84550.1")));
          } else if (field(m, "method") == "depth") {
            out.push_back(WsReply::send(snapshot_answer(field(m, "id"))));
          }
          return out;
        }};
    self = &server;
    const TempDir dir;
    const std::string text = config_text(dir);
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "sandbox.toml", {}, config, errors) == Status::Ok);

    live::SandboxRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "sandbox.toml";
    live::FeedEndpoints endpoints;
    endpoints.streams = server.url(""); // wss://localhost:<port>; the routes are appended
    endpoints.ws_api = server.url("/ws-fapi/v1");
    endpoints.tls.ca_file = server.ca_file();
    request.endpoints = endpoints;
    request.run_for = std::chrono::milliseconds{3000};

    // After the book syncs: more diffs in the chain, quotes and trades, as the venue streams.
    std::atomic<bool> pushed{false};
    std::thread later{[&server, &pushed] {
      std::size_t pub = 99;
      std::size_t mkt = 99;
      const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{5};
      while ((pub == 99 || mkt == 99) && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        for (std::size_t i = 0; i < 8; ++i) {
          const std::string t = server.target(i);
          pub = t.starts_with("/public/stream") ? i : pub;
          mkt = t.starts_with("/market/stream") ? i : mkt;
        }
      }
      if (pub == 99 || mkt == 99) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{600}); // the snapshot comes first
      server.push(pub, WsReply::send(diff(106, 110, 105, "",
                                          R"(["84550.1","0.0000"],["84550.3","1.0000"])")));
      server.push(pub, WsReply::send(book_ticker(1001, "84550.0", "84550.2")));
      server.push(mkt, WsReply::send(agg_trade(2, "84550.2")));
      std::this_thread::sleep_for(std::chrono::milliseconds{200});
      server.push(pub, WsReply::send(diff(111, 115, 110, R"(["84549.8","4.0000"])", "")));
      server.push(pub, WsReply::send(book_ticker(1002, "84550.0", "84550.2")));
      pushed = true;
    }};

    Tapper tapper;
    st::StaticStrategySet<Tapper> set{tapper};
    live::SandboxResult result;
    std::string error;
    node::NoHook hook;
    const Status s = live::run_sandbox(request, set, result, error, hook);
    later.join();
    INFO(error);
    CHECK(pushed.load());
    REQUIRE(s == Status::Ok);
    const Tapper& ran = set.get<0>();
    CHECK(result.feed.decode_errors == 0);
    CHECK(result.feed.snapshots >= 1);
    CHECK(result.feed.book_syncs >= 1);
    CHECK(ran.quotes >= 2);
    CHECK(ran.trades >= 2);
    CHECK(ran.books >= 2);
    CHECK(ran.fills >= 1);
    CHECK(ran.cancels == 1);
    CHECK(result.summary.venue_answers >= 4); // accepted, filled, accepted, canceled
    CHECK(result.summary.state == md::NodeState::Stopped);
    REQUIRE_FALSE(result.directory.empty());

    // The raw frames hold every message, snapshot answers included.
    live::RawFrameReader raw;
    REQUIRE(raw.open(result.directory + "/raw-frames.jraw", error) == Status::Ok);
    std::size_t frames = 0;
    live::RawFrame f;
    while (raw.next(f) == Status::Ok) {
      frames += f.kind == live::RawKind::Message ? 1 : 0;
    }
    CHECK(frames >= 9);

    // The raw frames rebuild the market data the run stepped, event for event.
    live::SandboxPlan plan;
    REQUIRE(live::plan_sandbox(request, UnixNanos{1}, plan, error) == Status::Ok);
    live::RedecodeResult redecoded;
    REQUIRE(live::redecode(result.directory + "/raw-frames.jraw", plan.feed.symbols, nullptr,
                           redecoded, error) == Status::Ok);
    CHECK(redecoded.decode_errors == 0);
    CHECK(redecoded.snapshots >= 1);
    std::size_t compared = 0;
    INFO(error);
    REQUIRE(live::check_market_inputs(result.directory, redecoded.events, compared, error) ==
            Status::Ok);
    CHECK(compared == result.summary.data_events);

    // The environment equivalence: the recorded session under the backtest wiring.
    st::StaticStrategySet<Tapper> fresh{Tapper{}};
    node::ReplayReport report;
    REQUIRE(node::replay_run(result.directory, config, fresh, node::ReplayOptions{}, report,
                             error) == Status::Ok);
    INFO((report.divergence ? report.divergence->recorded + " / " + report.divergence->replayed
                            : std::string{}));
    CHECK_FALSE(report.divergence.has_value());
    CHECK(report.inputs == result.summary.inputs);
    CHECK(report.outputs == result.summary.outputs);
    CHECK(server.error().empty());
  }
}
