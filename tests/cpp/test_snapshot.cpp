// The REST venue snapshot for reconciliation (jarvis/adapter/binance/snapshot.hpp): the
// decoders, and the assembly against a scripted futures host, including the consistent read.

#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/adapter/binance/rest_codec.hpp"
#include "jarvis/adapter/binance/snapshot.hpp"
#include "jarvis/adapter/codec.hpp"
#include "support/tls_test.hpp"

namespace {

namespace binance = jarvis::adapter::binance;
namespace m = jarvis::model;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::testsupport::ScriptedHttpsServer;

std::string http(std::string_view body, int status = 200) {
  return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Bad Request") +
         "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + std::string{body};
}

jarvis::adapter::SymbolTable symbols() {
  jarvis::adapter::SymbolTable t;
  m::InstrumentId id;
  REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  m::Currency usdt;
  REQUIRE(m::Currency::builtin("USDT", usdt) == Status::Ok);
  REQUIRE(t.add("BTCUSDT", jarvis::adapter::SymbolEntry{id, 1, 3, usdt}) == Status::Ok);
  return t;
}

m::AccountId account() {
  m::AccountId a;
  REQUIRE(m::AccountId::from("BINANCE-001", a) == Status::Ok);
  return a;
}

std::string order_json(std::string_view cid, std::uint64_t id, std::string_view status,
                       std::string_view executed, std::string_view tif = "GTC",
                       std::string_view symbol = "BTCUSDT") {
  return R"({"avgPrice":"0.00","clientOrderId":")" + std::string{cid} +
         R"(","cumQuote":"0","executedQty":")" + std::string{executed} + R"(","orderId":)" +
         std::to_string(id) +
         R"(,"origQty":"0.010","origType":"LIMIT","price":"65000.0","reduceOnly":false,"side":"BUY","positionSide":"BOTH","status":")" +
         std::string{status} + R"(","stopPrice":"0","closePosition":false,"symbol":")" +
         std::string{symbol} + R"(","time":1700000000100,"timeInForce":")" + std::string{tif} +
         R"(","type":"LIMIT","updateTime":1700000000200,"workingType":"CONTRACT_PRICE","priceProtect":false,"priceMatch":"NONE","selfTradePreventionMode":"NONE","goodTillDate":0})";
}

std::string trade_json(std::uint64_t id, std::uint64_t order_id, std::string_view qty,
                       std::uint64_t time) {
  return R"({"buyer":true,"commission":"0.02600000","commissionAsset":"USDT","id":)" +
         std::to_string(id) + R"(,"maker":true,"orderId":)" + std::to_string(order_id) +
         R"(,"price":"65000.0","qty":")" + std::string{qty} +
         R"(","quoteQty":"650.0","realizedPnl":"0","side":"BUY","positionSide":"BOTH","symbol":"BTCUSDT","time":)" +
         std::to_string(time) + "}";
}

const std::string kBalances =
    R"([{"accountAlias":"SgsR","asset":"USDT","balance":"1000.50000000","crossWalletBalance":"1000.5","crossUnPnl":"0","availableBalance":"900.25000000","maxWithdrawAmount":"900","marginAvailable":true,"updateTime":1700000000000},)"
    R"({"accountAlias":"SgsR","asset":"ZZZ","balance":"1","crossWalletBalance":"1","crossUnPnl":"0","availableBalance":"1","maxWithdrawAmount":"1","marginAvailable":true,"updateTime":0}])";

std::string positions_json(std::string_view amount) {
  return R"([{"symbol":"BTCUSDT","positionSide":"BOTH","positionAmt":")" + std::string{amount} +
         R"(","entryPrice":"64999.95","breakEvenPrice":"65000","markPrice":"65010.0","unRealizedProfit":"0.1","liquidationPrice":"0","isolatedMargin":"0","notional":"650","marginAsset":"USDT","isolatedWallet":"0","initialMargin":"32","maintMargin":"3","positionInitialMargin":"32","openOrderInitialMargin":"0","adl":1,"bidNotional":"0","askNotional":"0","updateTime":1700000000300},)"
         R"({"symbol":"ETHUSDT","positionSide":"BOTH","positionAmt":"1.000","entryPrice":"3000","updateTime":1}])";
}

binance::RestConfig config_for(const ScriptedHttpsServer& server) {
  binance::RestConfig c;
  c.base_url = server.url();
  c.tls.ca_file = server.ca_file();
  c.api_key = "KEY1";
  std::string error;
  REQUIRE(jarvis::network::Signer::from_secret("s3cret", c.signer, error) == Status::Ok);
  c.now_ms = [] { return std::int64_t{1'700'000'001'000}; };
  return c;
}

binance::AccountSnapshotRequest request() {
  binance::AccountSnapshotRequest r;
  r.account_id = account();
  r.symbols = {"BTCUSDT"};
  r.orders = {{"BTCUSDT", "jarvis-000001-00000001", 11},
              {"BTCUSDT", "jarvis-000001-00000002", 12},
              {"BTCUSDT", "jarvis-000001-00000003", 0}};
  r.trades_since_ms = 1'699'999'000'000;
  r.seed = 7;
  return r;
}

std::string target_of(const std::string& request) {
  const std::size_t start = request.find(' ') + 1;
  return request.substr(start, request.find('?', start) - start);
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("snapshot decoders: orders, balances, positions and trades") {
    const jarvis::adapter::SymbolTable table = symbols();
    const binance::ReportContext ctx{&table, account(), UnixNanos{5}};
    std::vector<m::OrderStatusReport> orders;
    std::size_t skipped = 0;
    std::string error;
    const std::string list = "[" + order_json("a-1", 11, "NEW", "0.000", "GTX") + "," +
                             order_json("a-2", 12, "PARTIALLY_FILLED", "0.004") + "," +
                             order_json("x", 99, "NEW", "0", "GTC", "DOGEUSDT") + "]";
    REQUIRE(binance::decode_order_reports(list, ctx, orders, skipped, error) == Status::Ok);
    REQUIRE(orders.size() == 2);
    CHECK(skipped == 1);
    CHECK(orders[0].client_order_id.value_or(m::ClientOrderId{}).view() == "a-1");
    CHECK(orders[0].venue_order_id.view() == "11");
    CHECK(orders[0].order_status == m::OrderStatus::Accepted);
    CHECK(orders[0].post_only);
    CHECK(orders[0].time_in_force == m::TimeInForce::Gtc);
    CHECK(orders[0].price.value_or(m::Price{}).raw() == 65000'000'000'000);
    CHECK(orders[0].ts_accepted == UnixNanos{1'700'000'000'100'000'000});
    CHECK(orders[0].ts_last == UnixNanos{1'700'000'000'200'000'000});
    CHECK(orders[1].order_status == m::OrderStatus::PartiallyFilled);
    CHECK(orders[1].filled_qty.raw() == 4'000'000);
    REQUIRE(binance::decode_order_reports(order_json("a-3", 13, "EXPIRED_IN_MATCH", "0"), ctx,
                                          orders, skipped, error) == Status::Ok);
    CHECK(orders.back().order_status == m::OrderStatus::Expired);

    std::vector<m::AccountBalance> balances;
    skipped = 0;
    REQUIRE(binance::decode_balances(kBalances, balances, skipped, error) == Status::Ok);
    REQUIRE(balances.size() == 1);
    CHECK(skipped == 1);
    CHECK(balances[0].total.raw() == 1000'500'000'000);
    CHECK(balances[0].free.raw() == 900'250'000'000);
    CHECK(balances[0].locked.raw() == 100'250'000'000);

    std::vector<m::PositionStatusReport> positions;
    skipped = 0;
    REQUIRE(binance::decode_positions(positions_json("-0.010"), ctx, positions, skipped, error) ==
            Status::Ok);
    REQUIRE(positions.size() == 1);
    CHECK(skipped == 1);
    CHECK(positions[0].position_side == m::PositionSide::Short);
    CHECK(positions[0].quantity.raw() == 10'000'000);
    CHECK(positions[0].avg_px_open.value_or(m::Price{}).raw() == 64999'950'000'000);
    positions.clear();
    REQUIRE(binance::decode_positions(positions_json("0.000"), ctx, positions, skipped, error) ==
            Status::Ok);
    CHECK(positions.empty()); // flat

    std::vector<m::FillReport> fills;
    std::uint64_t last = 0;
    std::size_t count = 0;
    REQUIRE(binance::decode_user_trades("[" + trade_json(501, 12, "0.004", 1'700'000'000'150) +
                                            "," + trade_json(502, 12, "0.001", 1'700'000'000'160) +
                                            "]",
                                        ctx, fills, last, count, error) == Status::Ok);
    REQUIRE(fills.size() == 2);
    CHECK(count == 2);
    CHECK(last == 502);
    CHECK(fills[0].trade_id.view() == "501");
    CHECK(fills[0].venue_order_id.view() == "12");
    CHECK(fills[0].commission.raw() == 26'000'000);
    CHECK(fills[0].liquidity_side == m::LiquiditySide::Maker);
    CHECK(fills[1].ts_event == UnixNanos{1'700'000'000'160'000'000});
  }

  TEST_CASE("a snapshot: open orders, the tracked ones' final state, then a consistent read") {
    ScriptedHttpsServer host{{
        http("[" + order_json("jarvis-000001-00000001", 11, "NEW", "0.000") + "]"),
        http(order_json("jarvis-000001-00000002", 12, "FILLED", "0.010")),
        http(R"({"code":-2013,"msg":"Order does not exist."})", 400),
        // Order 12 filled 0.010 but the adapter knows none of its trades: read by order.
        http("[" + trade_json(501, 12, "0.010", 1'700'000'000'150) + "]"),
        http("[" + trade_json(501, 12, "0.010", 1'700'000'000'150) + "]"), // trades (again)
        http(kBalances), http(positions_json("0.010")),
        http("[]"), // trades again: none, so the read is consistent
    }};
    binance::RestClient rest{config_for(host)};
    rest.set_time_offset(250);
    const jarvis::adapter::SymbolTable table = symbols();
    binance::AccountSnapshot snap;
    std::string error;
    REQUIRE(binance::assemble_snapshot(rest, table, request(), snap, error) == Status::Ok);
    CHECK(error.empty());
    CHECK(snap.ts_snapshot == UnixNanos{1'700'000'001'250'000'000}); // the venue's clock
    REQUIRE(snap.orders.size() == 2);
    CHECK(snap.orders[1].order_status == m::OrderStatus::Filled);
    REQUIRE(snap.fills.size() == 1);
    CHECK(snap.fills[0].client_order_id.value_or(m::ClientOrderId{}).view() ==
          "jarvis-000001-00000002");
    CHECK(snap.balances.size() == 1);
    REQUIRE(snap.positions.size() == 1);
    CHECK(snap.positions[0].position_side == m::PositionSide::Long);
    CHECK(snap.next_trade.at("BTCUSDT") == 502);
    CHECK(snap.rounds == 1);
    CHECK(snap.requests == 8);
    CHECK(snap.gap_reads == 1);
    CHECK(snap.skipped == 2); // ZZZ, ETHUSDT
    CHECK(!(snap.orders[0].report_id == snap.orders[1].report_id));

    const std::vector<std::string> requests = host.requests();
    REQUIRE(requests.size() == 8);
    CHECK(target_of(requests[0]) == "/fapi/v1/openOrders");
    CHECK(target_of(requests[1]) == "/fapi/v1/order");
    CHECK(requests[1].find("origClientOrderId=jarvis-000001-00000002") != std::string::npos);
    CHECK(requests[2].find("origClientOrderId=jarvis-000001-00000003") != std::string::npos);
    CHECK(target_of(requests[3]) == "/fapi/v1/userTrades");
    CHECK(requests[3].find("orderId=12") != std::string::npos);
    CHECK(target_of(requests[4]) == "/fapi/v1/userTrades");
    CHECK(requests[4].find("startTime=1699999000000") != std::string::npos);
    CHECK(target_of(requests[5]) == "/fapi/v3/balance");
    CHECK(target_of(requests[6]) == "/fapi/v3/positionRisk");
    CHECK(requests[7].find("fromId=502") != std::string::npos);

    const m::VenueSnapshot event = snap.event(UnixNanos{9});
    CHECK(event.orders.size() == 2);
    CHECK(event.fills.size() == 1);
    CHECK(event.ts_snapshot == snap.ts_snapshot);
    CHECK(event.ts_init == UnixNanos{9});
  }

  TEST_CASE("a light check reads the open orders and the positions only") {
    ScriptedHttpsServer host{{
        http("[" + order_json("jarvis-000001-00000001", 11, "NEW", "0.000") + "]"),
        http(positions_json("0.010")),
    }};
    binance::RestClient rest{config_for(host)};
    rest.set_time_offset(250);
    const jarvis::adapter::SymbolTable table = symbols();
    binance::AccountSnapshot snap;
    std::string error;
    REQUIRE(binance::assemble_check(rest, table, request(), snap, error) == Status::Ok);
    CHECK(snap.check);
    CHECK(snap.ts_snapshot == UnixNanos{1'700'000'001'250'000'000});
    CHECK(snap.orders.size() == 1);
    CHECK(snap.positions.size() == 1);
    CHECK(snap.fills.empty());
    CHECK(snap.balances.empty());
    CHECK(snap.requests == 2);
    const std::vector<std::string> requests = host.requests();
    REQUIRE(requests.size() == 2);
    CHECK(target_of(requests[0]) == "/fapi/v1/openOrders");
    CHECK(target_of(requests[1]) == "/fapi/v3/positionRisk");
    CHECK(snap.event(UnixNanos{9}).check);
  }

  TEST_CASE("trades arriving during the read make balances and positions be read again") {
    ScriptedHttpsServer host{{
        http("[]"),                                                        // open orders
        http("[]"),                                                        // trades: none yet
        http(kBalances), http(positions_json("0.000")),                    // round 1
        http("[" + trade_json(601, 77, "0.002", 1'700'000'000'500) + "]"), // a trade meanwhile
        http(kBalances), http(positions_json("0.002")),                    // round 2
        http("[]"),                                                        // consistent
    }};
    binance::RestClient rest{config_for(host)};
    const jarvis::adapter::SymbolTable table = symbols();
    binance::AccountSnapshotRequest req = request();
    req.orders.clear();
    req.next_trade["BTCUSDT"] = 600;
    binance::AccountSnapshot snap;
    std::string error;
    REQUIRE(binance::assemble_snapshot(rest, table, req, snap, error) == Status::Ok);
    CHECK(snap.rounds == 2);
    REQUIRE(snap.fills.size() == 1);
    CHECK(!snap.fills[0].client_order_id); // an order the node did not place
    REQUIRE(snap.positions.size() == 1);
    CHECK(snap.positions[0].quantity.raw() == 2'000'000);
    const std::vector<std::string> requests = host.requests();
    CHECK(requests[1].find("fromId=600") != std::string::npos);
    CHECK(requests[7].find("fromId=602") != std::string::npos);
  }

  TEST_CASE("a read that never settles fails, and venue errors pass through") {
    ScriptedHttpsServer busy{{
        http("[]"),
        http("[" + trade_json(1, 1, "0.001", 1) + "]"),
        http(kBalances),
        http(positions_json("0")),
        http("[" + trade_json(2, 1, "0.001", 2) + "]"),
    }};
    binance::RestClient rest{config_for(busy)};
    const jarvis::adapter::SymbolTable table = symbols();
    binance::AccountSnapshotRequest req = request();
    req.orders.clear();
    req.max_rounds = 1;
    binance::AccountSnapshot snap;
    std::string error;
    CHECK(binance::assemble_snapshot(rest, table, req, snap, error) == Status::WouldBlock);
    CHECK(error.find("trades kept arriving") != std::string::npos);

    ScriptedHttpsServer refusing{{http(R"({"code":-2015,"msg":"Invalid API-key."})", 401)}};
    binance::RestClient bad{config_for(refusing)};
    error.clear();
    CHECK(binance::assemble_snapshot(bad, table, req, snap, error) == Status::InvalidArgument);
    CHECK(error.find("-2015") != std::string::npos);
  }
}
