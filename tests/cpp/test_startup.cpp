// Startup checks (docs/architecture.md section 14.6) against scripted futures and spot hosts.

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/adapter/binance/startup.hpp"
#include "support/tls_test.hpp"

namespace {

namespace binance = jarvis::adapter::binance;
using jarvis::core::Status;
using jarvis::testsupport::ScriptedHttpsServer;

std::string http(std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

std::string exchange_info() {
  std::ifstream in{std::string{JARVIS_SOURCE_DIR} +
                   "/tests/data/binance/exchange_info_testnet.json"};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string position_risk(std::string_view symbol, std::string_view margin, int leverage) {
  return R"([{"symbol":")" + std::string{symbol} +
         R"(","positionAmt":"0.000","entryPrice":"0.0","markPrice":"65000.00","unRealizedProfit":"0.00000000","liquidationPrice":"0","leverage":")" +
         std::to_string(leverage) + R"(","maxNotionalValue":"1000000","marginType":")" +
         std::string{margin} +
         R"(","isolatedMargin":"0.00000000","isAutoAddMargin":"false","positionSide":"BOTH","notional":"0","isolatedWallet":"0","updateTime":0}])";
}

std::string restrictions(bool ip, bool futures, bool withdrawals) {
  const auto b = [](bool v) { return v ? "true" : "false"; };
  return std::string{R"({"ipRestrict":)"} + b(ip) +
         R"(,"createTime":1698645219000,"enableReading":true,"enableWithdrawals":)" +
         b(withdrawals) +
         R"(,"enableInternalTransfer":false,"enableMargin":false,"enableFutures":)" + b(futures) +
         R"(,"permitsUniversalTransfer":false,"enableSpotAndMarginTrading":false})";
}

binance::RestConfig config_for(const ScriptedHttpsServer& server) {
  binance::RestConfig c;
  c.base_url = server.url();
  c.tls.ca_file = server.ca_file();
  c.api_key = "KEY1";
  std::string error;
  REQUIRE(jarvis::network::Signer::from_secret("s3cret", c.signer, error) == Status::Ok);
  c.now_ms = [] { return std::int64_t{1'700'000'000'000}; };
  return c;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("startup checks pass on a matching account") {
    ScriptedHttpsServer futures_host{
        {http(R"({"serverTime":1700000000200})"), http(exchange_info()),
         http(R"({"dualSidePosition":false})"), http(R"({"multiAssetsMargin":false})"),
         http(position_risk("BTCUSDT", "cross", 20)), http(position_risk("EOSUSDT", "cross", 20))}};
    ScriptedHttpsServer spot_host{{http(restrictions(true, true, false))}};
    binance::RestClient futures{config_for(futures_host)};
    binance::RestClient spot{config_for(spot_host)};
    binance::StartupExpectations expect;
    expect.symbols = {"BTCUSDT", "EOSUSDT"};
    expect.leverage = 20;
    binance::StartupReport report;
    CHECK(binance::run_startup_checks(futures, &spot, expect, report) == Status::Ok);
    CHECK(report.passed());
    CHECK(report.time_offset_ms == 200);
    REQUIRE(report.instruments.size() == 2);
    CHECK(report.instruments[0].trading);
    CHECK_FALSE(report.instruments[1].trading);
    REQUIRE(report.warnings.size() == 1);
    CHECK(report.warnings[0] == "EOSUSDT is not TRADING; orders for it are refused");
    futures_host.join();
    spot_host.join();
    const auto spot_requests = spot_host.requests();
    REQUIRE(spot_requests.size() == 1);
    CHECK(spot_requests[0].starts_with("GET /sapi/v1/account/apiRestrictions?"));
    CHECK(spot_requests[0].find("timestamp=1700000000200") != std::string::npos); // venue clock
    CHECK(futures_host.error().empty());
    CHECK(spot_host.error().empty());
  }

  TEST_CASE("startup checks report every mismatch at once") {
    ScriptedHttpsServer futures_host{{http(R"({"serverTime":1700000005000})"),
                                      http(exchange_info()), http(R"({"dualSidePosition":true})"),
                                      http(R"({"multiAssetsMargin":true})"),
                                      http(position_risk("BTCUSDT", "isolated", 10))}};
    ScriptedHttpsServer spot_host{{http(restrictions(false, false, true))}};
    binance::RestClient futures{config_for(futures_host)};
    binance::RestClient spot{config_for(spot_host)};
    binance::StartupExpectations expect;
    expect.symbols = {"BTCUSDT"};
    expect.leverage = 20;
    binance::StartupReport report;
    CHECK(binance::run_startup_checks(futures, &spot, expect, report) == Status::InvalidState);
    CHECK(report.failures ==
          std::vector<std::string>{
              "the local clock is 5000 ms off the venue's (at most 1000 allowed)",
              "the account is in hedge mode; the node is configured for one-way",
              "multi-assets mode is on; the node is configured with it off",
              "BTCUSDT uses isolated margin; the node expects cross",
              "BTCUSDT leverage is 10; the node expects 20", "the API key may not trade futures",
              "the API key may withdraw; a trading key must not",
              "the API key has no IP whitelist"});
    CHECK(futures_host.error().empty());
    CHECK(spot_host.error().empty());
  }

  TEST_CASE("without a spot host the key check is a warning; unknown symbols fail") {
    ScriptedHttpsServer futures_host{{http(R"({"serverTime":1700000000000})"),
                                      http(exchange_info()), http(R"({"dualSidePosition":false})"),
                                      http(R"({"multiAssetsMargin":false})"),
                                      http(R"({"code":-1121,"msg":"Invalid symbol."})")}};
    binance::RestClient futures{config_for(futures_host)};
    binance::StartupExpectations expect;
    expect.symbols = {"DOGEUSDT"};
    binance::StartupReport report;
    CHECK(binance::run_startup_checks(futures, nullptr, expect, report) == Status::InvalidState);
    REQUIRE(report.failures.size() == 2);
    CHECK(report.failures[0].starts_with("exchangeInfo: "));
    CHECK(report.failures[1].starts_with("positionRisk DOGEUSDT: "));
    CHECK(report.warnings ==
          std::vector<std::string>{"API key permissions not checked (no spot API host)"});
    CHECK(futures_host.error().empty());
  }
}
