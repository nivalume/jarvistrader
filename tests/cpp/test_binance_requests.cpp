// Requests to the USDⓈ-M trading endpoints (jarvis/adapter/binance/requests.hpp) and the REST
// client (rest_client.hpp) against a scripted loopback HTTPS venue: parameters from kernel
// commands, signing, answers, rate headers, 418 bans and the order fallback's outcomes.

#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/requests.hpp"
#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/network/crypto.hpp"
#include "jarvis/network/signer.hpp"
#include "support/tls_test.hpp"

namespace {

namespace binance = jarvis::adapter::binance;
namespace model = jarvis::model;
namespace net = jarvis::network;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

template <typename T> T parsed(std::string_view text) {
  T v;
  REQUIRE(T::parse(text, v) == Status::Ok);
  return v;
}

model::SubmitOrder limit_buy() {
  model::SubmitOrder c;
  REQUIRE(model::ClientOrderId::from("jarvis-000001-00000001", c.client_order_id) == Status::Ok);
  REQUIRE(binance::perpetual_id("BTCUSDT", c.instrument_id) == Status::Ok);
  c.order_side = model::OrderSide::Buy;
  c.order_type = model::OrderType::Limit;
  c.quantity = parsed<model::Quantity>("0.010");
  c.price = parsed<model::Price>("65000.1");
  return c;
}

std::string param(const binance::Params& p, std::string_view key) {
  for (const auto& [k, v] : p) {
    if (k == key) {
      return v;
    }
  }
  return "<none>";
}

std::string http(int status, std::string_view body, std::string_view extra = "") {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\n" +
         std::string{extra} + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
         std::string{body};
}

// The query string of a recorded request line ("GET /path?query HTTP/1.1").
std::string query_of(const std::string& request) {
  const std::size_t q = request.find('?');
  const std::size_t end = request.find(' ', q);
  return q == std::string::npos ? std::string{} : request.substr(q + 1, end - q - 1);
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("kernel commands become order parameters") {
    binance::Params p;
    std::string error;
    REQUIRE(binance::place_params(limit_buy(), "BTCUSDT", p, error) == Status::Ok);
    CHECK(param(p, "type") == "LIMIT");
    CHECK(param(p, "timeInForce") == "GTC");
    CHECK(param(p, "price") == "65000.1");
    CHECK(param(p, "quantity") == "0.010");
    CHECK(param(p, "newClientOrderId") == "jarvis-000001-00000001");
    CHECK(param(p, "reduceOnly") == "<none>");
    CHECK(param(p, "newOrderRespType") == "ACK");

    model::SubmitOrder post = limit_buy();
    post.post_only = true;
    post.reduce_only = true;
    REQUIRE(binance::place_params(post, "BTCUSDT", p, error) == Status::Ok);
    CHECK(param(p, "timeInForce") == "GTX");
    CHECK(param(p, "reduceOnly") == "true");

    model::SubmitOrder market = limit_buy();
    market.order_type = model::OrderType::Market;
    market.price.reset();
    REQUIRE(binance::place_params(market, "BTCUSDT", p, error) == Status::Ok);
    CHECK(param(p, "type") == "MARKET");
    CHECK(param(p, "price") == "<none>");

    model::SubmitOrder gtd = limit_buy();
    gtd.time_in_force = model::TimeInForce::Gtd;
    CHECK(binance::place_params(gtd, "BTCUSDT", p, error) == Status::InvalidArgument);

    model::ModifyOrder m;
    m.client_order_id = limit_buy().client_order_id;
    m.quantity = parsed<model::Quantity>("0.020");
    m.price = parsed<model::Price>("64999.9");
    REQUIRE(binance::modify_params(m, "BTCUSDT", model::OrderSide::Sell, p, error) == Status::Ok);
    CHECK(param(p, "side") == "SELL");
    CHECK(param(p, "origClientOrderId") == "jarvis-000001-00000001");
    CHECK(param(p, "price") == "64999.9");
  }

  TEST_CASE("REST queries are signed as sent; WebSocket API logons sign sorted parameters") {
    CHECK(binance::url_encode("a b+c/d=") == "a%20b%2Bc%2Fd%3D");
    net::Signer hmac;
    std::string error;
    REQUIRE(net::Signer::from_secret("s3cret", hmac, error) == Status::Ok);
    const std::string q = binance::signed_query({{"symbol", "BTCUSDT"}, {"side", "BUY"}},
                                                1'700'000'000'000, 5000, hmac);
    const std::string body = "symbol=BTCUSDT&side=BUY&recvWindow=5000&timestamp=1700000000000";
    CHECK(q == body + "&signature=" + net::hex(net::hmac_sha256("s3cret", body)));

    const std::string pem = "-----BEGIN PRIVATE KEY-----\n"
                            "MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n"
                            "-----END PRIVATE KEY-----\n";
    net::Signer ed;
    REQUIRE(net::Signer::from_secret(pem, ed, error) == Status::Ok);
    const std::string logon =
        binance::ws_request("L1", "session.logon", {}, 1649729878532, &ed, "KEY1");
    const std::string payload = "apiKey=KEY1&timestamp=1649729878532";
    CHECK(
        logon ==
        R"({"id":"L1","method":"session.logon","params":{"apiKey":"KEY1","timestamp":1649729878532,"signature":")" +
            ed.sign(payload) + R"("}})");
    // After the logon, requests carry only a timestamp.
    CHECK(
        binance::ws_request("7", "order.cancel",
                            {{"symbol", "BTCUSDT"}, {"origClientOrderId", "C-1"}}, 5, nullptr) ==
        R"({"id":"7","method":"order.cancel","params":{"symbol":"BTCUSDT","origClientOrderId":"C-1","timestamp":5}})");
    // An Ed25519 REST signature is percent-encoded.
    const std::string edq = binance::signed_query({}, 1, 5000, ed);
    CHECK(edq.find("signature=") != std::string::npos);
    CHECK(edq.substr(edq.find("signature=") + 10).find_first_of("+/=") == std::string::npos);
  }

  TEST_CASE("WebSocket API answers: acknowledgements, errors and rate limits") {
    binance::WsAnswer a;
    std::string error;
    REQUIRE(
        binance::decode_ws_answer(
            R"({"id":"p1","status":200,"result":{"orderId":325078477,"symbol":"BTCUSDT","status":"NEW","clientOrderId":"C-1","updateTime":1702995765087},"rateLimits":[{"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","intervalNum":1,"limit":2400,"count":1},{"rateLimitType":"ORDERS","interval":"SECOND","intervalNum":10,"limit":300,"count":1},{"rateLimitType":"ORDERS","interval":"MINUTE","intervalNum":1,"limit":1200,"count":1}]})",
            UnixNanos{9}, a, error) == Status::Ok);
    CHECK(a.id == "p1");
    CHECK(a.status == 200);
    REQUIRE(a.ack.has_value());
    CHECK(a.ack.value_or(binance::PlaceAck{}).order_id == 325078477);
    CHECK(a.ack.value_or(binance::PlaceAck{}).client_order_id == "C-1");
    REQUIRE(a.limits.size() == 3);
    CHECK(a.limits[0].kind == model::RateLimitKind::RequestWeight);
    CHECK(a.limits[0].interval_ns == 60'000'000'000ULL);
    CHECK(a.limits[1].kind == model::RateLimitKind::Orders);
    CHECK(a.limits[1].interval_ns == 10'000'000'000ULL);
    CHECK(a.limits[1].limit == 300);
    CHECK(a.limits[2].ts_init == UnixNanos{9});

    REQUIRE(
        binance::decode_ws_answer(
            R"({"id":"p2","status":400,"error":{"code":-2010,"msg":"Account has insufficient balance for requested action."}})",
            UnixNanos{9}, a, error) == Status::Ok);
    CHECK(a.status == 400);
    CHECK(a.error_code == -2010);
    CHECK_FALSE(a.ack.has_value());

    std::vector<model::RateLimitFeedback> fb;
    binance::feedback_from_header("X-MBX-USED-WEIGHT-1M", "7", UnixNanos{1}, fb);
    binance::feedback_from_header("x-mbx-order-count-10s", "3", UnixNanos{1}, fb);
    binance::feedback_from_header("Content-Type", "x", UnixNanos{1}, fb);
    REQUIRE(fb.size() == 2);
    CHECK(fb[0].kind == model::RateLimitKind::RequestWeight);
    CHECK(fb[0].used == 7);
    CHECK(fb[1].interval_ns == 10'000'000'000ULL);
  }

  TEST_CASE("the REST client against a scripted venue") {
    jarvis::testsupport::ScriptedHttpsServer venue{{
        http(200, R"({"serverTime":1700000000500})"),
        http(200, R"({"dualSidePosition":false})", "X-MBX-USED-WEIGHT-1M: 31\r\n"),
        http(
            200,
            R"([{"symbol":"BTCUSDT","positionAmt":"0.000","entryPrice":"0.0","leverage":"20","marginType":"cross"}])"),
        http(200,
             R"({"listenKey":"pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1"})"),
        http(
            200,
            R"({"orderId":22542179,"clientOrderId":"jarvis-000001-00000001","status":"NEW","updateTime":1700000000600})",
            "X-MBX-ORDER-COUNT-10S: 1\r\nX-MBX-ORDER-COUNT-1M: 1\r\n"),
        http(400, R"({"code":-2019,"msg":"Margin is insufficient."})"),
        http(
            503,
            R"({"code":-1001,"msg":"Internal error; unable to process your request. Please try again."})"),
        http(418, R"({"code":-1003,"msg":"Way too many requests; IP banned."})",
             "Retry-After: 60\r\n"),
    }};
    net::Signer hmac;
    std::string error;
    REQUIRE(net::Signer::from_secret("s3cret", hmac, error) == Status::Ok);
    std::int64_t clock = 1'700'000'000'000;
    binance::RestConfig cfg;
    cfg.base_url = venue.url();
    cfg.tls.ca_file = venue.ca_file();
    cfg.api_key = "KEY1";
    cfg.signer = hmac;
    cfg.now_ms = [&clock] { return clock; };
    binance::RestClient rest{cfg};

    REQUIRE(rest.sync_time(error) == Status::Ok);
    CHECK(rest.time_offset_ms() == 500); // the venue runs half a second ahead
    bool dual = true;
    REQUIRE(rest.position_mode(dual, error) == Status::Ok);
    CHECK_FALSE(dual);
    binance::PositionRisk risk;
    REQUIRE(rest.position_risk("BTCUSDT", risk, error) == Status::Ok);
    CHECK(risk.leverage == 20);
    CHECK(risk.margin_type == "cross");
    std::string key;
    REQUIRE(rest.create_listen_key(key, error) == Status::Ok);
    CHECK(key.starts_with("pqia91"));

    binance::Params order;
    REQUIRE(binance::place_params(limit_buy(), "BTCUSDT", order, error) == Status::Ok);
    binance::PlaceAck ack;
    binance::RequestError refusal;
    bool refused = true;
    REQUIRE(rest.place(order, ack, refusal, refused, error) == Status::Ok);
    CHECK_FALSE(refused);
    CHECK(ack.order_id == 22542179);
    REQUIRE(rest.place(order, ack, refusal, refused, error) == Status::Ok);
    CHECK(refused);
    CHECK(refusal.code == -2019);
    CHECK(refusal.client_order_id == "jarvis-000001-00000001");
    CHECK(rest.place(order, ack, refusal, refused, error) == Status::IoError); // 5xx: unknown
    CHECK(error.find("unknown") != std::string::npos);

    CHECK_FALSE(rest.banned());
    CHECK(rest.depth("BTCUSDT", 5, key, error) == Status::InvalidArgument); // the 418
    CHECK(rest.banned());
    CHECK(rest.depth("BTCUSDT", 5, key, error) == Status::InvalidState); // fails fast
    clock += 61'000;
    CHECK_FALSE(rest.banned());

    const std::vector<model::RateLimitFeedback> limits = rest.take_limits();
    REQUIRE(limits.size() == 3);
    CHECK(limits[0].used == 31);
    CHECK(limits[1].kind == model::RateLimitKind::Orders);

    venue.join();
    CHECK(venue.error().empty());
    const std::vector<std::string> requests = venue.requests();
    REQUIRE(requests.size() == 8);
    CHECK(requests[0].starts_with("GET /fapi/v1/time HTTP/1.1"));
    CHECK(jarvis::testsupport::header_value(requests[1], "X-MBX-APIKEY") == "KEY1");
    // The signed query carries the venue's time and verifies with the secret.
    const std::string q = query_of(requests[1]);
    CHECK(q.find("timestamp=1700000000500") != std::string::npos);
    const std::size_t sig = q.find("&signature=");
    REQUIRE(sig != std::string::npos);
    CHECK(q.substr(sig + 11) == net::hex(net::hmac_sha256("s3cret", q.substr(0, sig))));
    CHECK(requests[3].starts_with("POST /fapi/v1/listenKey HTTP/1.1"));
    CHECK(query_of(requests[3]).empty()); // API key only, no signature
    CHECK(requests[4].starts_with("POST /fapi/v1/order?symbol=BTCUSDT&side=BUY&type=LIMIT"));
  }
}
