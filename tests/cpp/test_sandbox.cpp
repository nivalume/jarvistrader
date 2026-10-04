// The sandbox node end to end against a scripted venue (docs/architecture.md sections 4.1 and
// 4.6): the market feed thread syncs a book from diffs and a WebSocket API snapshot, a strategy
// trades on the simulated exchange in real time, and the recorded run replays under the
// backtest wiring with the same outputs, byte for byte (the environment equivalence test).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/live/admin_server.hpp"
#include "jarvis/live/market_feed.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/redecode.hpp"
#include "jarvis/live/sandbox_node.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/admin_protocol.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/node/snapshot_file.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "support/http_get.hpp"
#include "support/ws_test.hpp"

namespace {

namespace live = jarvis::live;
namespace md = jarvis::model;
namespace wire = jarvis::model::wire;
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
  std::vector<std::string> params; // what on_params_changed received: key=text (kind)

  void on_params_changed(st::Context& /*ctx*/, const md::ParamUpdate& p) {
    params.push_back(std::string{p.key.view()} + "=" + std::string{p.text.view()} + " (" +
                     std::string{md::to_string(p.kind)} + ")");
  }

  // Set once it has seen all the streaming test sends: three quotes, two trades, three book
  // updates (the snapshot and two diffs), and its resting order canceled.
  std::atomic<bool>* stop = nullptr;

  void check_done() const {
    if (stop != nullptr && quotes >= 3 && trades >= 2 && books >= 3 && cancels == 1) {
      stop->store(true);
    }
  }

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
    check_done();
    if (bought) {
      return Status::Ok;
    }
    bought = true;
    md::Quantity qty;
    static_cast<void>(md::Quantity::parse("0.0010", qty));
    md::ClientOrderId id;
    return ctx.submit(ctx.market(btc(), md::OrderSide::Buy, qty), id);
  }
  void on_trade(st::Context& /*ctx*/, const md::TradeTick& /*t*/) {
    ++trades;
    check_done();
  }
  void on_book(st::Context& /*ctx*/, const jarvis::data::BookView& /*b*/) {
    ++books;
    check_done();
  }
  Status on_order_event(st::Context& ctx, const md::OrderEvent& e) {
    if (std::holds_alternative<md::OrderCanceled>(e)) {
      ++cancels;
      check_done();
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

// Counts the trades it sees and stops the node at `want`.
struct TradeCounter {
  int trades = 0;
  int want = 2;
  std::atomic<bool>* stop = nullptr;

  static Status on_start(st::Context& ctx) { return ctx.subscribe_trades(btc()); }
  void on_trade(st::Context& /*ctx*/, const md::TradeTick& /*t*/) {
    if (++trades >= want && stop != nullptr) {
      stop->store(true);
    }
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
  TEST_CASE("the admin socket takes commands, answers status, and is its owner's only") {
    namespace fs = std::filesystem;
    const TempDir dir;
    const std::string path = dir.file("admin.sock");
    live::AdminServer server{path};
    server.set_strategies({"tap-001", "mm-002"});
    std::string error;
    REQUIRE(server.start(error) == Status::Ok);
    CHECK((fs::status(path).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
          fs::perms::none);
    std::string reply;
    REQUIRE(node::admin_request(path, "halt", reply, error) == Status::Ok);
    CHECK(reply == "ok");
    REQUIRE(node::admin_request(path, "bogus", reply, error) == Status::Ok);
    CHECK(reply.starts_with("error unknown command"));
    node::AdminRequest request;
    REQUIRE(server.commands().try_pop(request));
    CHECK(request.action == md::AdminAction::Halt);
    CHECK_FALSE(request.is_param);
    CHECK_FALSE(server.commands().try_pop(request));
    CHECK(server.accepted() == 1);

    // set_param names a strategy by id; the value is typed when it can be.
    REQUIRE(node::admin_request(path, "set_param mm-002 spread_bps 3", reply, error) == Status::Ok);
    CHECK(reply == "ok");
    REQUIRE(node::admin_request(path, "set_param tap-001 mode \"aggressive now\"", reply, error) ==
            Status::Ok);
    CHECK(reply == "ok");
    REQUIRE(node::admin_request(path, "set_param nobody x 1", reply, error) == Status::Ok);
    CHECK(reply == "error no strategy \"nobody\" in this node");
    REQUIRE(node::admin_request(path, "set_param mm-002 x", reply, error) == Status::Ok);
    CHECK(reply == "error set_param needs a strategy id, a key and a value");
    REQUIRE(node::admin_request(path, "snapshot", reply, error) == Status::Ok);
    CHECK(reply == "ok");
    REQUIRE(server.commands().try_pop(request));
    CHECK(request.is_param);
    CHECK(request.param.strategy_index == 1);
    CHECK(request.param.key.view() == "spread_bps");
    CHECK(request.param.kind == md::ParamKind::Int);
    CHECK(request.param.integer == 3);
    REQUIRE(server.commands().try_pop(request));
    CHECK(request.param.strategy_index == 0);
    CHECK(request.param.kind == md::ParamKind::Text);
    CHECK(request.param.text.view() == "aggressive now");
    REQUIRE(server.commands().try_pop(request));
    CHECK_FALSE(request.is_param);
    CHECK(request.action == md::AdminAction::Snapshot);
    CHECK(server.accepted() == 4);

    REQUIRE(node::admin_request(path, "status", reply, error) == Status::Ok);
    CHECK(reply.find(R"("ready":false,"alive":false)") != std::string::npos); // nothing published
    server.status().publish(md::NodeState::Running, md::TradingState::Reducing, 42,
                            jarvis::network::steady_ns());
    REQUIRE(node::admin_request(path, "status", reply, error) == Status::Ok);
    CHECK(reply ==
          R"({"state":"RUNNING","trading":"REDUCING","seq":42,"ready":true,"alive":true})");

    server.stop();
    CHECK_FALSE(fs::exists(path));
    CHECK(node::admin_request(path, "status", reply, error) == Status::IoError);
  }

  TEST_CASE("an operator halts and stops a sandbox node through its admin socket") {
    std::atomic<std::uint64_t> trades{0};
    ScriptedWssServer server{8, [&trades](std::size_t /*conn*/, const std::string& m) {
                               std::vector<WsReply> out;
                               if (m.empty()) {
                                 out.push_back(WsReply::send(agg_trade(++trades, "84550.1")));
                               }
                               return out;
                             }};
    const TempDir dir;
    std::string text = config_text(dir);
    const std::string all = R"(streams = ["aggTrade", "bookTicker", "depth@100ms"])";
    text.replace(text.find(all), all.size(), R"(streams = ["aggTrade"])");
    const std::string socket = dir.file("admin.sock");
    text += "\n[admin]\nsocket = \"unix://" + socket + "\"\n";
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "sandbox.toml", {}, config, errors) == Status::Ok);
    live::SandboxRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "sandbox.toml";
    live::FeedEndpoints endpoints;
    endpoints.streams = server.url("");
    endpoints.ws_api = server.url("/ws-fapi/v1");
    endpoints.tls.ca_file = server.ca_file();
    request.endpoints = endpoints;
    request.run_for = std::chrono::seconds{20};

    // The operator: waits for the node to run, halts it, sees the state change, stops it.
    std::vector<std::string> replies;
    std::thread operator_thread{[&socket, &replies] {
      const auto wait_for = [&socket](std::string_view needle) {
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        std::string reply;
        std::string error;
        while (std::chrono::steady_clock::now() < give_up) {
          if (jarvis::core::ok(node::admin_request(socket, "status", reply, error)) &&
              reply.find(needle) != std::string::npos) {
            return reply;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return std::string{};
      };
      std::string reply;
      std::string error;
      replies.push_back(wait_for(R"("state":"RUNNING")"));
      static_cast<void>(node::admin_request(socket, "set_param tap-001 size 5", reply, error));
      replies.push_back(reply);
      static_cast<void>(node::admin_request(socket, "snapshot", reply, error));
      replies.push_back(reply);
      static_cast<void>(node::admin_request(socket, "halt", reply, error));
      replies.push_back(reply);
      replies.push_back(wait_for(R"("trading":"HALTED")"));
      static_cast<void>(node::admin_request(socket, "shutdown", reply, error));
      replies.push_back(reply);
    }};
    st::StaticStrategySet<Tapper> set{Tapper{}};
    live::SandboxResult result;
    std::string error;
    node::NoHook hook;
    const auto started = std::chrono::steady_clock::now();
    const Status s = live::run_sandbox(request, set, result, error, hook);
    operator_thread.join();
    INFO(error);
    REQUIRE(s == Status::Ok);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{15});
    REQUIRE(replies.size() == 6);
    CHECK(replies[0].find(R"("ready":true,"alive":true)") != std::string::npos);
    CHECK(replies[1] == "ok"); // set_param
    CHECK(replies[2] == "ok"); // snapshot
    CHECK(replies[3] == "ok"); // halt
    CHECK_FALSE(replies[4].empty());
    CHECK(replies[5] == "ok");
    CHECK(result.admin_commands == 4);
    CHECK(result.summary.state == md::NodeState::Stopped);
    CHECK(set.get<0>().params == std::vector<std::string>{"size=5 (INT)"});
    // The requested snapshot (snapshot_every is far off), written at the next batch end, and
    // the final one after the Stopped transition.
    CHECK(result.summary.snapshots == 2);
    CHECK(node::list_snapshots(result.directory).size() == 2);

    // The commands are recorded inputs, and the session replays with the same outputs, the same
    // parameter and the same snapshot.
    node::EventLogReader reader;
    REQUIRE(reader.open(result.directory) == Status::Ok);
    wire::RecordView v;
    int admins = 0;
    int params = 0;
    while (reader.next(v) == Status::Ok) {
      admins += v.header.kind == static_cast<std::uint16_t>(wire::RecordKind::AdminCommand) ? 1 : 0;
      params += v.header.kind == static_cast<std::uint16_t>(wire::RecordKind::ParamUpdate) ? 1 : 0;
    }
    CHECK(admins == 3);
    CHECK(params == 1);
    st::StaticStrategySet<Tapper> fresh{Tapper{}};
    node::ReplayReport report;
    REQUIRE(node::replay_run(result.directory, config, fresh, node::ReplayOptions{}, report,
                             error) == Status::Ok);
    CHECK_FALSE(report.divergence.has_value());
    CHECK(report.outputs == result.summary.outputs);
    CHECK(report.snapshots_checked == 2);
    CHECK(fresh.get<0>().params == set.get<0>().params);
  }

  TEST_CASE("the market feed records its stream connection going down and back up") {
    ScriptedWssServer* self = nullptr;
    std::atomic<int> opens{0};
    ScriptedWssServer server{8, [&self, &opens](std::size_t conn, const std::string& m) {
                               std::vector<WsReply> out;
                               if (m.empty() && self->target(conn).starts_with("/market/stream")) {
                                 if (opens++ == 0) {
                                   out.push_back(WsReply::send(agg_trade(1, "84550.1")));
                                   out.push_back(WsReply::drop());
                                 } else {
                                   out.push_back(WsReply::send(agg_trade(2, "84550.2")));
                                 }
                               }
                               return out;
                             }};
    self = &server;
    const TempDir dir;
    std::string text = config_text(dir);
    const std::string all = R"(streams = ["aggTrade", "bookTicker", "depth@100ms"])";
    text.replace(text.find(all), all.size(), R"(streams = ["aggTrade"])");
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "sandbox.toml", {}, config, errors) == Status::Ok);
    live::SandboxRequest request;
    request.config = &config;
    live::FeedEndpoints endpoints;
    endpoints.streams = server.url("");
    endpoints.ws_api = server.url("/ws-fapi/v1");
    endpoints.tls.ca_file = server.ca_file();
    request.endpoints = endpoints;
    live::SandboxPlan plan;
    std::string error;
    REQUIRE(live::plan_sandbox(request, UnixNanos{1}, plan, error) == Status::Ok);
    plan.feed.reconnect_initial = std::chrono::milliseconds{20};

    const live::ArrivalClock anchor;
    live::MarketFeed feed{anchor, plan.feed};
    REQUIRE(feed.start(error) == Status::Ok);
    std::vector<std::string> seen;
    wire::DecodeScratch scratch{16};
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (seen.size() < 5 && std::chrono::steady_clock::now() < give_up) {
      bool empty = false;
      const std::span<const std::byte> record = feed.ring().peek(empty);
      if (empty) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      wire::RecordView v;
      md::Event e;
      REQUIRE(wire::decode_record(record, v) == Status::Ok);
      REQUIRE(wire::decode_event(v, scratch, e) == Status::Ok);
      if (const auto* c = std::get_if<md::ConnectionStatus>(&e)) {
        CHECK(c->kind == md::ConnectionKind::MarketData);
        CHECK(c->venue.view() == "BINANCE");
        seen.emplace_back(c->up ? "up" : "down");
      } else if (std::holds_alternative<md::TradeTick>(e)) {
        seen.emplace_back("trade");
      }
      feed.ring().release();
    }
    feed.stop();
    CHECK(seen == std::vector<std::string>{"up", "trade", "down", "up", "trade"});
    CHECK(server.error().empty());
  }

  TEST_CASE("the market feed reconnects and resyncs the book after a stream goes silent") {
    // The depth connection delivers a diff and a quote and then goes silent without closing (no
    // FIN, no RST): only the idle deadline can notice. The other connections are quiet but
    // answer pings, so they must live. After the reconnect the book syncs again from a fresh
    // snapshot and the new connection's data arrives.
    ScriptedWssServer* self = nullptr;
    std::atomic<int> public_opens{0};
    std::atomic<int> snapshots{0};
    ScriptedWssServer server{
        10, [&self, &public_opens, &snapshots](std::size_t conn, const std::string& m) {
          const std::string target = self->target(conn);
          std::vector<WsReply> out;
          if (m.empty() && target.starts_with("/public/stream")) {
            out.push_back(WsReply::send(diff(95, 105, 90, R"(["84550.0","1.2000"])", "")));
            if (public_opens++ == 0) {
              out.push_back(WsReply::send(book_ticker(1000, "84550.0", "84550.1")));
              out.push_back(WsReply::silent());
            } else {
              out.push_back(WsReply::send(book_ticker(2000, "84551.0", "84551.1")));
            }
          } else if (m.empty() && target.starts_with("/market/stream")) {
            out.push_back(WsReply::send(agg_trade(1, "84550.1")));
          } else if (field(m, "method") == "depth") {
            ++snapshots;
            out.push_back(WsReply::send(snapshot_answer(field(m, "id"))));
          }
          return out;
        }};
    self = &server;
    const TempDir dir;
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(config_text(dir), "sandbox.toml", {}, config, errors) == Status::Ok);
    live::SandboxRequest request;
    request.config = &config;
    live::FeedEndpoints endpoints;
    endpoints.streams = server.url("");
    endpoints.ws_api = server.url("/ws-fapi/v1");
    endpoints.tls.ca_file = server.ca_file();
    request.endpoints = endpoints;
    live::SandboxPlan plan;
    std::string error;
    REQUIRE(live::plan_sandbox(request, UnixNanos{1}, plan, error) == Status::Ok);
    CHECK(plan.feed.idle_timeout == std::chrono::seconds{30}); // the default
    CHECK(plan.feed.api_idle_timeout == std::chrono::seconds{60});
    plan.feed.reconnect_initial = std::chrono::milliseconds{20};
    plan.feed.idle_timeout = std::chrono::milliseconds{500};
    plan.feed.api_idle_timeout = std::chrono::milliseconds{500};

    const live::ArrivalClock anchor;
    live::MarketFeed feed{anchor, plan.feed};
    REQUIRE(feed.start(error) == Status::Ok);
    std::vector<std::string> status; // ConnectionStatus(MarketData), in order
    int clears = 0;
    int quotes_after_up_again = 0;
    wire::DecodeScratch scratch{4096};
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while ((quotes_after_up_again < 1 || feed.stats().snapshots < 2 || clears < 2) &&
           std::chrono::steady_clock::now() < give_up) {
      bool empty = false;
      const std::span<const std::byte> record = feed.ring().peek(empty);
      if (empty) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      wire::RecordView v;
      md::Event e;
      REQUIRE(wire::decode_record(record, v) == Status::Ok);
      REQUIRE(wire::decode_event(v, scratch, e) == Status::Ok);
      if (const auto* c = std::get_if<md::ConnectionStatus>(&e)) {
        status.emplace_back(c->up ? "up" : "down");
      } else if (const auto* d = std::get_if<md::OrderBookDeltas>(&e)) {
        clears += (!d->deltas.empty() && d->deltas.front().action == md::BookAction::Clear) ? 1 : 0;
      } else if (std::holds_alternative<md::QuoteTick>(e) && status.size() >= 3 &&
                 status.back() == "up") {
        ++quotes_after_up_again;
      }
      feed.ring().release();
    }
    const live::MarketFeedStats stats = feed.stats();
    feed.stop();
    CHECK(status == std::vector<std::string>{"up", "down", "up"});
    CHECK(quotes_after_up_again == 1);
    CHECK(stats.idle_timeouts == 1); // the depth connection only; the quiet ones answered pings
    CHECK(stats.connects == 3);      // two stream connections, the depth one twice
    CHECK(stats.snapshots >= 2);     // the book synced from a fresh snapshot after the stall
    CHECK(snapshots.load() >= 2);
    CHECK(clears >= 2); // the book was cleared when it synced and again when its stream closed
    CHECK(server.error().empty());
  }

  TEST_CASE("a sandbox node degrades on a silent stall and returns to Running") {
    // The stream connection says one trade and goes silent. With [network] market_idle_timeout_ms
    // = 1000 the client gives up, the sync gate takes the node to Degraded when the connection
    // is recorded down, and the reconnect (a second trade) brings it back through Syncing.
    ScriptedWssServer* self = nullptr;
    std::atomic<int> opens{0};
    ScriptedWssServer server{
        8, [&self, &opens](std::size_t conn, const std::string& m) {
          std::vector<WsReply> out;
          if (m.empty() && self->target(conn).starts_with("/market/stream")) {
            const int n = ++opens;
            out.push_back(WsReply::send(agg_trade(static_cast<std::uint64_t>(n), "84550.1")));
            if (n == 1) {
              out.push_back(WsReply::silent());
            }
          }
          return out;
        }};
    self = &server;
    const TempDir dir;
    std::string text = config_text(dir);
    const std::string all = R"(streams = ["aggTrade", "bookTicker", "depth@100ms"])";
    text.replace(text.find(all), all.size(), R"(streams = ["aggTrade"])");
    text.replace(text.find("cpp:Tapper"), 10, "cpp:TradeCounter");
    text += "\n[network]\nmarket_idle_timeout_ms = 1000\n";
    node::NodeConfig config;
    std::vector<node::ConfigError> errors;
    REQUIRE(node::parse_config(text, "sandbox.toml", {}, config, errors) == Status::Ok);
    CHECK(config.network.market_idle_timeout_ms == 1000);
    CHECK(config.network.venue_idle_timeout_ms == 60'000);
    live::SandboxRequest request;
    request.config = &config;
    request.manifest.config_text = text;
    request.manifest.source = "sandbox.toml";
    live::FeedEndpoints endpoints;
    endpoints.streams = server.url("");
    endpoints.ws_api = server.url("/ws-fapi/v1");
    endpoints.tls.ca_file = server.ca_file();
    request.endpoints = endpoints;
    std::atomic<bool> stop{false};
    request.stop = &stop;
    request.run_for = std::chrono::seconds{30};

    TradeCounter counter;
    counter.stop = &stop;
    st::StaticStrategySet<TradeCounter> set{counter};
    live::SandboxResult result;
    std::string error;
    node::NoHook hook;
    const auto started = std::chrono::steady_clock::now();
    const Status s = live::run_sandbox(request, set, result, error, hook);
    INFO(error);
    REQUIRE(s == Status::Ok);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{15});
    CHECK(set.get<0>().trades == 2);
    CHECK(result.feed.idle_timeouts == 1);
    CHECK(result.feed.connects == 2);
    CHECK(result.summary.state == md::NodeState::Stopped);

    // The lifecycle in the telemetry lines: Running, then Degraded, then Running again.
    std::ifstream jsonl{result.directory + "/telemetry.jsonl"};
    std::vector<std::string> states;
    for (std::string line; std::getline(jsonl, line);) {
      if (line.find(R"("event":"NodeLifecycle")") == std::string::npos) {
        continue;
      }
      INFO(line);
      for (const std::string_view name : {"RUNNING", "DEGRADED", "SYNCING"}) {
        if (line.find(R"("to":")" + std::string{name} + "\"") != std::string::npos) {
          states.emplace_back(name);
        }
      }
    }
    const auto first = [&states](std::string_view n, std::size_t from) {
      return std::find(states.begin() + static_cast<std::ptrdiff_t>(from), states.end(), n);
    };
    const auto running = first("RUNNING", 0);
    REQUIRE(running != states.end());
    const auto degraded = first("DEGRADED", static_cast<std::size_t>(running - states.begin()));
    REQUIRE(degraded != states.end());
    CHECK(first("RUNNING", static_cast<std::size_t>(degraded - states.begin())) != states.end());
    CHECK(server.error().empty());
  }

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
    const std::string text = config_text(dir) + "\n[telemetry]\nprometheus = \"127.0.0.1:0\"\n";
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
    // The strategy stops the node once it has seen everything below; the bound only ends a run
    // that never gets there (under TSan the node starts slowly).
    std::atomic<bool> stop{false};
    request.stop = &stop;
    request.run_for = std::chrono::seconds{30};
    std::atomic<std::uint16_t> telemetry_port{0};
    request.telemetry_port = &telemetry_port;

    // After the book syncs: more diffs in the chain, quotes and trades, as the venue streams.
    // Between them the Prometheus endpoint is scraped.
    std::atomic<bool> pushed{false};
    std::string metrics;
    int metrics_status = 0;
    int ready_status = 0;
    std::thread later{[&server, &pushed, &telemetry_port, &metrics, &metrics_status,
                       &ready_status] {
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
      metrics = jarvis::testsupport::http_get(telemetry_port.load(), "/metrics", metrics_status);
      static_cast<void>(
          jarvis::testsupport::http_get(telemetry_port.load(), "/ready", ready_status));
      server.push(pub, WsReply::send(diff(111, 115, 110, R"(["84549.8","4.0000"])", "")));
      server.push(pub, WsReply::send(book_ticker(1002, "84550.0", "84550.2")));
      pushed = true;
    }};

    Tapper tapper;
    tapper.stop = &stop;
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

    // Telemetry: scraped while running, and the JSON lines of the session beside its log.
    CHECK(metrics_status == 200);
    CHECK(ready_status == 200);
    CHECK(metrics.find("jarvis_node_state{state=\"RUNNING\"} 1") != std::string::npos);
    CHECK(metrics.find("jarvis_ring_capacity{ring=\"market\"}") != std::string::npos);
    CHECK(metrics.find("jarvis_market_data_age_ns ") != std::string::npos);
    CHECK(metrics.find("jarvis_feed_connects_total ") != std::string::npos);
    CHECK(result.telemetry_dropped == 0);
    std::ifstream jsonl{result.directory + "/telemetry.jsonl"};
    std::vector<std::string> lines;
    for (std::string line; std::getline(jsonl, line);) {
      lines.push_back(line);
    }
    CHECK(lines.size() == result.telemetry_lines);
    const auto count = [&lines](std::string_view needle) {
      return std::count_if(lines.begin(), lines.end(), [needle](const std::string& l) {
        return l.find(needle) != std::string::npos;
      });
    };
    CHECK(count(R"("event":"NodeLifecycle")") >= 4);
    CHECK(count(R"("event":"ConnectionStatus")") >= 1);
    CHECK(count(R"("event":"SubmitOrder")") == 2);
    CHECK(count(R"("event":"OrderFilled")") == 1);
    CHECK(count(R"("event":"OrderCanceled")") == 1);
    CHECK(count(R"("event":"trading_state")") >= 1);
    for (const std::string& l : lines) {
      CHECK(l.starts_with("{\"ts\":"));
      CHECK(l.ends_with("}"));
    }

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
    // Every feed input but its ConnectionStatus(MarketData) up, which no frame decodes to.
    CHECK(compared + 1 == result.summary.data_events);

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
