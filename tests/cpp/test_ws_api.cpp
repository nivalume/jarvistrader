// The WebSocket API session (docs/architecture.md section 14.4) against a scripted loopback
// venue: logon, request correlation, refusals, timeouts, lost connections, rotation.

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/ws_api.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/signer.hpp"
#include "support/ws_test.hpp"

namespace {

namespace binance = jarvis::adapter::binance;
namespace model = jarvis::model;
namespace net = jarvis::network;
using jarvis::core::Status;
using jarvis::testsupport::ScriptedWssServer;
using jarvis::testsupport::WsReply;

constexpr std::string_view kEdPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n"
    "-----END PRIVATE KEY-----\n";

// A string field of a flat JSON request: "key":"value".
std::string field(const std::string& json, std::string_view key) {
  const std::string open = "\"" + std::string{key} + "\":\"";
  const std::size_t at = json.find(open);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + open.size();
  return json.substr(start, json.find('"', start) - start);
}

bool has(const std::string& json, std::string_view key) {
  return json.find("\"" + std::string{key} + "\":") != std::string::npos;
}

std::string ok(const std::string& id, std::string_view result) {
  return R"({"id":")" + id + R"(","status":200,"result":)" + std::string{result} +
         R"(,"rateLimits":[{"rateLimitType":"ORDERS","interval":"SECOND","intervalNum":10,"limit":300,"count":7}]})";
}

std::string refused(const std::string& id, int code, std::string_view msg) {
  return R"({"id":")" + id + R"(","status":400,"error":{"code":)" + std::to_string(code) +
         R"(,"msg":")" + std::string{msg} + R"("}})";
}

std::string order_result(std::string_view coid, std::uint64_t order_id) {
  return R"({"orderId":)" + std::to_string(order_id) +
         R"(,"symbol":"BTCUSDT","status":"NEW","clientOrderId":")" + std::string{coid} +
         R"(","updateTime":1700000000123})";
}

model::SubmitOrder limit_buy(std::string_view coid) {
  model::SubmitOrder c;
  REQUIRE(model::ClientOrderId::from(coid, c.client_order_id) == Status::Ok);
  REQUIRE(binance::perpetual_id("BTCUSDT", c.instrument_id) == Status::Ok);
  c.order_side = model::OrderSide::Buy;
  c.order_type = model::OrderType::Limit;
  REQUIRE(model::Quantity::parse("0.010", c.quantity) == Status::Ok);
  model::Price px;
  REQUIRE(model::Price::parse("65000.1", px) == Status::Ok);
  c.price = px;
  return c;
}

// Runs the loop until `done` or the deadline; true when `done`.
bool run_until(net::IoContext& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = std::chrono::milliseconds{5000}) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > end) {
      return false;
    }
    io.restart();
    io.run_for(std::chrono::milliseconds{5});
  }
  return true;
}

struct Recorder {
  std::vector<binance::OrderOutcome> outcomes;
  std::vector<std::string> downs;
  std::vector<model::RateLimitFeedback> limits;
  int readies = 0;

  binance::WsApiHandlers handlers() {
    binance::WsApiHandlers h;
    h.on_ready = [this] { ++readies; };
    h.on_down = [this](const std::string& r) { downs.push_back(r); };
    h.on_outcome = [this](const binance::OrderOutcome& o) { outcomes.push_back(o); };
    h.on_limits = [this](std::span<const model::RateLimitFeedback> l) {
      limits.insert(limits.end(), l.begin(), l.end());
    };
    return h;
  }
};

binance::WsApiConfig config_for(const ScriptedWssServer& server, bool ed25519) {
  binance::WsApiConfig c;
  c.url = server.url("/ws-fapi/v1");
  c.tls.ca_file = server.ca_file();
  c.api_key = "KEY1";
  std::string error;
  REQUIRE(net::Signer::from_secret(ed25519 ? std::string{kEdPem} : std::string{"s3cret"}, c.signer,
                                   error) == Status::Ok);
  c.now_ms = [] { return std::int64_t{1'700'000'000'000}; };
  c.reconnect_initial = std::chrono::milliseconds{20};
  c.reconnect_max = std::chrono::milliseconds{100};
  return c;
}

// Logs on and answers order requests: refuses client order ids ending in "R", ignores "-S"
// (silent), answers everything else with an acknowledgement.
std::vector<WsReply> venue(std::size_t /*conn*/, const std::string& m) {
  if (m.empty()) {
    return {};
  }
  const std::string id = field(m, "id");
  const std::string method = field(m, "method");
  if (method == "session.logon") {
    return {WsReply::send(ok(id, R"({"apiKey":"KEY1","authorizedSince":1700000000000})"))};
  }
  const std::string coid =
      field(m, method == "order.place" ? "newClientOrderId" : "origClientOrderId");
  if (coid.ends_with("R")) {
    return {WsReply::send(refused(id, -5022, "Due to the order could not be executed as maker"))};
  }
  if (coid.ends_with("S")) {
    return {};
  }
  if (method == "userDataStream.start") {
    return {WsReply::send(ok(id, R"({"listenKey":"LK1"})"))};
  }
  return {WsReply::send(ok(id, order_result(coid, 42)))};
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the WebSocket API session logs on, then correlates answers to requests") {
    ScriptedWssServer server{1, venue};
    net::IoContext io;
    Recorder rec;
    binance::WsApiSession session{io, config_for(server, true), rec.handlers()};
    std::string error;
    CHECK(session.place(limit_buy("jarvis-A"), "BTCUSDT", error) == Status::InvalidState);
    session.start();
    REQUIRE(run_until(io, [&] { return session.ready(); }));
    CHECK(rec.readies == 1);

    REQUIRE(session.place(limit_buy("jarvis-A"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(session.place(limit_buy("jarvis-R"), "BTCUSDT", error) == Status::Ok);
    model::CancelOrder cancel;
    REQUIRE(model::ClientOrderId::from("jarvis-A", cancel.client_order_id) == Status::Ok);
    REQUIRE(session.cancel(cancel, "BTCUSDT", error) == Status::Ok);
    std::string listen_key;
    REQUIRE(session.request(
                "userDataStream.start", {}, binance::Security::ApiKey,
                [&](Status s, const binance::WsAnswer& a, std::string_view json) {
                  CHECK(s == Status::Ok);
                  CHECK(a.status == 200);
                  listen_key = std::string{json};
                },
                error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 3 && !listen_key.empty(); }));

    CHECK(rec.outcomes[0].kind == binance::OutcomeKind::Acknowledged);
    CHECK(rec.outcomes[0].request == binance::RequestKind::Place);
    CHECK(rec.outcomes[0].ack.order_id == 42);
    CHECK(rec.outcomes[0].ack.status == "NEW");
    CHECK(rec.outcomes[1].kind == binance::OutcomeKind::Refused);
    CHECK(rec.outcomes[1].error.code == -5022);
    CHECK(rec.outcomes[1].error.client_order_id == "jarvis-R");
    CHECK(rec.outcomes[2].kind == binance::OutcomeKind::Acknowledged);
    CHECK(rec.outcomes[2].request == binance::RequestKind::Cancel);
    CHECK(listen_key.find("LK1") != std::string::npos);
    CHECK(rec.limits.size() == 4); // one per acknowledged answer, the logon's included
    CHECK(rec.limits[0].kind == model::RateLimitKind::Orders);
    CHECK(rec.limits[0].used == 7);

    const auto messages = server.messages();
    REQUIRE(messages.size() == 5);
    const std::string& logon = messages[0].second;
    CHECK(field(logon, "method") == "session.logon");
    CHECK(field(logon, "apiKey") == "KEY1");
    CHECK(has(logon, "signature"));
    const std::string& place = messages[1].second;
    CHECK(field(place, "method") == "order.place");
    CHECK(has(place, "timestamp"));
    CHECK(has(place, "recvWindow"));
    CHECK_FALSE(has(place, "signature")); // logged on
    CHECK_FALSE(has(place, "apiKey"));
    const std::string& start = messages[4].second;
    CHECK(field(start, "apiKey") == "KEY1");
    CHECK_FALSE(has(start, "timestamp"));
    CHECK(server.targets() == std::vector<std::string>{"/ws-fapi/v1"});

    const binance::WsApiStats st = session.stats();
    CHECK(st.sent == 4);
    CHECK(st.acknowledged == 2);
    CHECK(st.refused == 1);
    CHECK(st.unmatched == 0);
    session.stop();
    REQUIRE(run_until(io, [&] { return !rec.downs.empty(); }));
    CHECK(server.error().empty());
  }

  TEST_CASE("an HMAC key signs every WebSocket API request") {
    ScriptedWssServer server{1, venue};
    net::IoContext io;
    Recorder rec;
    binance::WsApiSession session{io, config_for(server, false), rec.handlers()};
    session.start();
    REQUIRE(run_until(io, [&] { return session.ready(); }));
    std::string error;
    REQUIRE(session.place(limit_buy("jarvis-A"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 1; }));
    const auto messages = server.messages();
    REQUIRE(messages.size() == 1); // no logon
    CHECK(field(messages[0].second, "apiKey") == "KEY1");
    CHECK(has(messages[0].second, "signature"));
    CHECK(server.error().empty());
  }

  TEST_CASE("unanswered and lost requests end as unknown; late answers still count") {
    std::string held;
    ScriptedWssServer server{2, [&](std::size_t conn, const std::string& m) {
                               const std::string coid = field(m, "newClientOrderId");
                               if (coid == "jarvis-S") {
                                 held = field(m, "id");
                                 return std::vector<WsReply>{};
                               }
                               if (coid == "jarvis-L") { // answers the held one late, then this
                                 return std::vector<WsReply>{
                                     WsReply::send(ok(held, order_result("jarvis-S", 7))),
                                     WsReply::send(ok(field(m, "id"), order_result(coid, 8)))};
                               }
                               if (coid == "jarvis-D" && conn == 0) {
                                 return std::vector<WsReply>{WsReply::drop()};
                               }
                               return venue(conn, m);
                             }};
    net::IoContext io;
    Recorder rec;
    binance::WsApiConfig config = config_for(server, true);
    config.request_timeout = std::chrono::milliseconds{150};
    binance::WsApiSession session{io, config, rec.handlers()};
    session.start();
    REQUIRE(run_until(io, [&] { return session.ready(); }));
    std::string error;
    REQUIRE(session.place(limit_buy("jarvis-S"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 1; }));
    CHECK(rec.outcomes[0].kind == binance::OutcomeKind::Unknown);
    CHECK(rec.outcomes[0].reason == "timeout");
    CHECK(rec.outcomes[0].client_order_id == "jarvis-S");

    REQUIRE(session.place(limit_buy("jarvis-L"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 3; }));
    CHECK(rec.outcomes[1].kind == binance::OutcomeKind::Acknowledged);
    CHECK(rec.outcomes[1].client_order_id == "jarvis-S");
    CHECK(rec.outcomes[1].ack.order_id == 7);
    CHECK(rec.outcomes[2].ack.order_id == 8);
    CHECK(session.stats().late_answers == 1);
    CHECK(session.stats().timeouts == 1);

    // The venue drops the connection with the request in flight: unknown, then a reconnect
    // and a new logon.
    REQUIRE(session.place(limit_buy("jarvis-D"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 4; }));
    CHECK(rec.outcomes[3].kind == binance::OutcomeKind::Unknown);
    CHECK(rec.outcomes[3].client_order_id == "jarvis-D");
    CHECK(rec.downs.size() == 1);
    CHECK_FALSE(session.ready());
    CHECK(session.place(limit_buy("jarvis-X"), "BTCUSDT", error) == Status::InvalidState);
    REQUIRE(run_until(io, [&] { return session.ready(); }));
    CHECK(rec.readies == 2);
    REQUIRE(session.place(limit_buy("jarvis-D"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 5; }));
    CHECK(rec.outcomes[4].kind == binance::OutcomeKind::Acknowledged);
    CHECK(session.stats().lost == 1);
    CHECK(session.stats().connects == 2);
    CHECK(server.error().empty());
  }

  TEST_CASE("a planned rotation opens the next connection before closing the old one") {
    ScriptedWssServer server{2, venue};
    net::IoContext io;
    Recorder rec;
    binance::WsApiConfig config = config_for(server, true);
    config.rotate_after = std::chrono::milliseconds{200};
    binance::WsApiSession session{io, config, rec.handlers()};
    session.start();
    REQUIRE(run_until(io, [&] { return session.ready(); }));
    REQUIRE(run_until(io, [&] { return session.stats().rotations == 1 && server.served() == 1; }));
    CHECK(session.ready());
    std::string error;
    REQUIRE(session.place(limit_buy("jarvis-A"), "BTCUSDT", error) == Status::Ok);
    REQUIRE(run_until(io, [&] { return rec.outcomes.size() == 1; }));
    const auto messages = server.messages();
    REQUIRE(messages.size() == 3); // logon, logon, place
    CHECK(messages[1].first == 1);
    CHECK(messages[2].first == 1); // the order went to the new connection
    CHECK(rec.downs.empty());      // order entry never paused
    CHECK(rec.readies == 1);
    session.stop();
    CHECK(server.error().empty());
  }

  TEST_CASE("a refused logon stops the session") {
    ScriptedWssServer server{1, [](std::size_t, const std::string& m) {
                               if (m.empty()) {
                                 return std::vector<WsReply>{};
                               }
                               return std::vector<WsReply>{WsReply::send(
                                   refused(field(m, "id"), -2015, "Invalid API-key"))};
                             }};
    net::IoContext io;
    Recorder rec;
    binance::WsApiSession session{io, config_for(server, true), rec.handlers()};
    session.start();
    REQUIRE(run_until(io, [&] { return !rec.downs.empty(); }));
    CHECK(rec.downs[0] == "session.logon refused: -2015 Invalid API-key");
    REQUIRE(run_until(io, [&] { return server.served() == 1; }));
    io.restart();
    io.run_for(std::chrono::milliseconds{200}); // no reconnect follows
    CHECK(session.stats().connects == 1);
    CHECK_FALSE(session.ready());
    CHECK(rec.readies == 0);
    CHECK(server.error().empty());
  }
}
