// The venue-io thread (jarvis/live/venue_io.hpp) against a scripted venue: the WebSocket API and
// the user data stream over WSS, the listenKey and the reconciliation snapshot over HTTPS.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/venue_io.hpp"
#include "jarvis/model/wire.hpp"
#include "support/tls_test.hpp"
#include "support/ws_test.hpp"

namespace {

namespace binance = jarvis::adapter::binance;
namespace live = jarvis::live;
namespace m = jarvis::model;
namespace wire = jarvis::model::wire;
using jarvis::core::Status;
using jarvis::testsupport::ScriptedHttpsServer;
using jarvis::testsupport::ScriptedWssServer;
using jarvis::testsupport::WsReply;

template <typename Id> Id make(std::string_view text) {
  Id id;
  REQUIRE(Id::from(text, id) == Status::Ok);
  return id;
}

std::string http(std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

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

std::string subscribe_id(const std::string& m) {
  const std::size_t at = m.find("\"id\":");
  return m.substr(at + 5, m.find('}', at) - at - 5);
}

std::string order_update(std::string_view cid, std::string_view x, std::uint64_t trade) {
  return R"({"stream":"LK1","data":{"e":"ORDER_TRADE_UPDATE","E":1700000001100,"T":1700000001099,"o":{"s":"BTCUSDT","c":")" +
         std::string{cid} +
         R"(","S":"BUY","o":"LIMIT","f":"GTC","q":"0.010","p":"65000.0","ap":"65000.0","sp":"0","x":")" +
         std::string{x} +
         R"(","X":"PARTIALLY_FILLED","i":42,"l":"0.004","z":"0.004","L":"65000.0","N":"USDT","n":"0.052","T":1700000001090,"t":)" +
         std::to_string(trade) +
         R"(,"b":"0","a":"0","m":true,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0"}}})";
}

const std::string kBalances =
    R"([{"accountAlias":"a","asset":"USDT","balance":"1000.0","crossWalletBalance":"1000","crossUnPnl":"0","availableBalance":"1000.0","maxWithdrawAmount":"1000","marginAvailable":true,"updateTime":1700000000000}])";

const std::string kOpenC1 =
    R"([{"avgPrice":"65000","clientOrderId":"C-1","cumQuote":"260","executedQty":"0.004","orderId":42,"origQty":"0.010","origType":"LIMIT","price":"65000.0","reduceOnly":false,"side":"BUY","positionSide":"BOTH","status":"PARTIALLY_FILLED","stopPrice":"0","closePosition":false,"symbol":"BTCUSDT","time":1700000001000,"timeInForce":"GTC","type":"LIMIT","updateTime":1700000001090,"workingType":"CONTRACT_PRICE","priceProtect":false,"priceMatch":"NONE","selfTradePreventionMode":"NONE","goodTillDate":0}])";

// The WebSocket API acknowledges every order; the user stream confirms subscriptions.
std::vector<WsReply> venue(const ScriptedWssServer& server, std::size_t conn,
                           const std::string& message) {
  if (message.empty()) {
    return {};
  }
  if (server.target(conn) == "/private/stream") {
    return {WsReply::send(R"({"result":null,"id":)" + subscribe_id(message) + "}")};
  }
  const std::string id = field(message, "id");
  const std::string cid = field(message, "newClientOrderId");
  return {WsReply::send(
      R"({"id":")" + id +
      R"(","status":200,"result":{"orderId":42,"symbol":"BTCUSDT","status":"NEW","clientOrderId":")" +
      cid +
      R"(","updateTime":1700000001000},"rateLimits":[{"rateLimitType":"ORDERS","interval":"SECOND","intervalNum":10,"limit":300,"count":1}]})")};
}

struct Record {
  std::string kind;
  m::Event event;
};

// Drains the ring into kinds (and the events, whose borrowed spans are not kept).
class Reader {
public:
  explicit Reader(live::SpscByteRing& ring) : ring_{&ring}, scratch_{4096} {}

  void drain() {
    for (;;) {
      bool empty = false;
      const std::span<const std::byte> bytes = ring_->peek(empty);
      if (empty) {
        return;
      }
      wire::RecordView view;
      REQUIRE(wire::decode_record(bytes, view) == Status::Ok);
      m::Event e;
      scratch_.deltas.clear();
      scratch_.balances.clear();
      scratch_.margins.clear();
      scratch_.orders.clear();
      scratch_.fills.clear();
      scratch_.positions.clear();
      REQUIRE(wire::decode_event(view, scratch_, e) == Status::Ok);
      std::string kind{wire::kind_name(wire::kind_of(e))};
      if (const auto* c = std::get_if<m::ConnectionStatus>(&e)) {
        kind += std::string{" "} + std::string{m::to_string(c->kind)} + (c->up ? " up" : " down");
      } else if (const auto* v = std::get_if<m::VenueSnapshot>(&e)) {
        kind += " orders=" + std::to_string(v->orders.size()) +
                " balances=" + std::to_string(v->balances.size());
      }
      records.push_back(Record{kind, e});
      ring_->release();
    }
  }

  // Waits until a record of `kind` (a prefix) has arrived since the index `from`.
  bool wait_for(std::string_view kind, std::size_t from = 0,
                std::chrono::milliseconds limit = std::chrono::milliseconds{8000}) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      drain();
      for (std::size_t i = from; i < records.size(); ++i) {
        if (records[i].kind.starts_with(kind)) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return false;
  }

  [[nodiscard]] std::vector<std::string> kinds() const {
    std::vector<std::string> out;
    for (const Record& r : records) {
      if (!r.kind.starts_with("RateLimitFeedback")) {
        out.push_back(r.kind);
      }
    }
    return out;
  }

  std::vector<Record> records;

private:
  live::SpscByteRing* ring_;
  wire::DecodeScratch scratch_;
};

live::VenueIoConfig config_for(const ScriptedWssServer& wss, const ScriptedHttpsServer& https) {
  live::VenueIoConfig c;
  c.endpoints.rest = https.url();
  c.endpoints.ws_api = wss.url("/ws-fapi/v1");
  c.endpoints.user_stream = wss.url("/private/stream");
  c.endpoints.tls.ca_file = wss.ca_file(); // the REST host serves the same certificate
  c.api_key = "KEY1";
  std::string error;
  REQUIRE(jarvis::network::Signer::from_secret("s3cret", c.signer, error) == Status::Ok);
  m::InstrumentId id;
  REQUIRE(binance::perpetual_id("BTCUSDT", id) == Status::Ok);
  m::Currency usdt;
  REQUIRE(m::Currency::builtin("USDT", usdt) == Status::Ok);
  REQUIRE(c.symbols.add("BTCUSDT", jarvis::adapter::SymbolEntry{id, 1, 3, usdt}) == Status::Ok);
  c.identity.trader_id = make<m::TraderId>("jarvis-001");
  c.identity.account_id = make<m::AccountId>("BINANCE-001");
  c.identity.strategies = {make<m::StrategyId>("mm-001")};
  c.identity.seed = 3;
  c.venue = make<m::Venue>("BINANCE");
  c.trades_since_ms = 1'700'000'000'000;
  c.reconnect_initial = std::chrono::milliseconds{20};
  c.reconnect_max = std::chrono::milliseconds{100};
  c.snapshot_retry = std::chrono::milliseconds{50};
  c.rest_tick = std::chrono::milliseconds{20};
  c.now_ms = [] { return std::int64_t{1'700'000'002'000}; };
  return c;
}

m::SubmitOrder limit_buy(std::string_view cid) {
  m::SubmitOrder c;
  c.client_order_id = make<m::ClientOrderId>(cid);
  REQUIRE(binance::perpetual_id("BTCUSDT", c.instrument_id) == Status::Ok);
  c.order_side = m::OrderSide::Buy;
  c.order_type = m::OrderType::Limit;
  REQUIRE(m::Quantity::parse("0.010", c.quantity) == Status::Ok);
  m::Price px;
  REQUIRE(m::Price::parse("65000.0", px) == Status::Ok);
  c.price = px;
  return c;
}

std::size_t stream_conn(const ScriptedWssServer& server, std::size_t skip = 0) {
  for (std::size_t conn = 0; conn < server.accepted(); ++conn) {
    if (server.target(conn) == "/private/stream" && skip-- == 0) {
      return conn;
    }
  }
  return 99;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("venue-io: sync, orders, fills and a reconnect, in order on one ring") {
    std::atomic<ScriptedWssServer*> server{nullptr}; // the script runs on the server's threads
    ScriptedWssServer wss{4, [&server](std::size_t conn, const std::string& message) {
                            return venue(*server.load(), conn, message);
                          }};
    server.store(&wss);
    ScriptedHttpsServer https{
        std::vector<std::string>{
            http(R"({"listenKey":"LK1"})"),
            // The first snapshot: nothing open, no trades.
            http("[]"),
            http("[]"),
            http(kBalances),
            http("[]"),
            http("[]"),
            // After the reconnect: C-1 is open with the fill the stream reported.
            http(kOpenC1),
            http(
                R"([{"buyer":true,"commission":"0.052","commissionAsset":"USDT","id":7,"maker":true,"orderId":42,"price":"65000.0","qty":"0.004","quoteQty":"260","realizedPnl":"0","side":"BUY","positionSide":"BOTH","symbol":"BTCUSDT","time":1700000001090}])"),
            http(kBalances),
            http(
                R"([{"symbol":"BTCUSDT","positionSide":"BOTH","positionAmt":"0.004","entryPrice":"65000.0","updateTime":1700000001090}])"),
            http("[]"),
        },
        wss.ca_file(), wss.key_file()};
    const live::ArrivalClock clock;
    live::VenueIo io{clock, config_for(wss, https)};
    std::string error;
    REQUIRE(io.start(error) == Status::Ok);
    Reader reader{io.ring()};

    REQUIRE(reader.wait_for("VenueSnapshot"));
    CHECK(reader.kinds() == std::vector<std::string>{"ConnectionStatus ORDER_ENTRY up",
                                                     "ConnectionStatus USER_STREAM up",
                                                     "VenueSnapshot orders=0 balances=1"});

    // An order: acknowledged on the WebSocket API, filled on the user stream.
    REQUIRE(io.commands().try_push(live::VenueCommand{limit_buy("C-1")}));
    REQUIRE(reader.wait_for("OrderAccepted"));
    const std::size_t conn = stream_conn(wss);
    wss.push(conn, WsReply::send(order_update("C-1", "TRADE", 7)));
    REQUIRE(reader.wait_for("OrderFilled"));

    // A modify of an order the venue-io never sent is refused locally.
    m::ModifyOrder modify;
    modify.client_order_id = make<m::ClientOrderId>("C-9");
    modify.instrument_id = limit_buy("C-9").instrument_id;
    REQUIRE(io.commands().try_push(live::VenueCommand{modify}));
    REQUIRE(reader.wait_for("OrderModifyRejected"));

    // The stream drops: down, then up again on a new connection, and a second snapshot.
    const std::size_t before = reader.records.size();
    wss.push(conn, WsReply::drop());
    REQUIRE(reader.wait_for("ConnectionStatus USER_STREAM down", before));
    REQUIRE(reader.wait_for("VenueSnapshot", before));
    std::vector<std::string> after = reader.kinds();
    after.erase(after.begin(), after.begin() + 6);
    CHECK(after == std::vector<std::string>{"ConnectionStatus USER_STREAM down",
                                            "ConnectionStatus USER_STREAM up",
                                            "VenueSnapshot orders=1 balances=1"});
    const auto& snap = std::get<m::VenueSnapshot>(reader.records.back().event);
    CHECK(snap.ts_snapshot.value() == 1'700'000'002'000'000'000ULL);

    io.stop();
    const live::VenueIoStats stats = io.stats();
    CHECK(stats.snapshots == 2);
    CHECK(stats.commands == 2);
    CHECK(stats.refused_locally == 1);
    CHECK(stats.decode_errors == 0);
    CHECK(io.last_error().empty());

    // The REST calls, in order; the second snapshot asked for C-1's trades from the one it saw.
    const std::vector<std::string> requests = https.requests();
    REQUIRE(requests.size() == 11);
    CHECK(requests[0].starts_with("POST /fapi/v1/listenKey"));
    CHECK(requests[2].find("startTime=1700000000000") != std::string::npos);
    CHECK(requests[6].starts_with("GET /fapi/v1/openOrders"));
    CHECK(requests[7].find("fromId=8") != std::string::npos);
    CHECK(wss.error().empty());
  }
}
