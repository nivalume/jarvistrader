#include "jarvis/adapter/binance/startup.hpp"

#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace jarvis::adapter::binance {

namespace {

using core::Status;

std::string joined(std::initializer_list<std::string_view> parts) {
  std::string out;
  for (const std::string_view p : parts) {
    out += p;
  }
  return out;
}

void check_instruments(RestClient& futures, const StartupExpectations& expect,
                       StartupReport& report) {
  std::string body;
  std::string error;
  if (!core::ok(futures.exchange_info(body, error))) {
    report.failures.push_back("exchangeInfo: " + error);
    return;
  }
  const core::UnixNanos ts{static_cast<std::uint64_t>(futures.now_ms()) * 1'000'000};
  if (!core::ok(parse_exchange_info(body, expect.symbols, ts, report.instruments, error))) {
    report.failures.push_back("exchangeInfo: " + error);
    return;
  }
  for (const PerpetualDefinition& d : report.instruments) {
    if (!d.trading) {
      report.warnings.push_back(std::string{d.instrument.common.raw_symbol.view()} +
                                " is not TRADING; orders for it are refused");
    }
  }
}

void check_account(RestClient& futures, const StartupExpectations& expect, StartupReport& report) {
  std::string error;
  bool dual = false;
  if (!core::ok(futures.position_mode(dual, error))) {
    report.failures.push_back("positionSide/dual: " + error);
  } else if (dual != expect.hedge_mode) {
    report.failures.push_back(std::string{"the account is in "} + (dual ? "hedge" : "one-way") +
                              " mode; the node is configured for " +
                              (expect.hedge_mode ? "hedge" : "one-way"));
  }
  bool multi = false;
  if (!core::ok(futures.multi_assets_mode(multi, error))) {
    report.failures.push_back("multiAssetsMargin: " + error);
  } else if (multi != expect.multi_assets) {
    report.failures.push_back(std::string{"multi-assets mode is "} + (multi ? "on" : "off") +
                              "; the node is configured with it " +
                              (expect.multi_assets ? "on" : "off"));
  }
  for (const std::string& symbol : expect.symbols) {
    PositionRisk risk;
    if (!core::ok(futures.position_risk(symbol, risk, error))) {
      report.failures.push_back(joined({"positionRisk ", symbol, ": ", error}));
      continue;
    }
    if (risk.margin_type != expect.margin_type) {
      report.failures.push_back(joined(
          {symbol, " uses ", risk.margin_type, " margin; the node expects ", expect.margin_type}));
    }
    if (expect.leverage != 0 && risk.leverage != expect.leverage) {
      report.failures.push_back(joined({symbol, " leverage is ", std::to_string(risk.leverage),
                                        "; the node expects ", std::to_string(expect.leverage)}));
    }
  }
}

void check_key(RestClient* spot, const StartupExpectations& expect, StartupReport& report) {
  if (spot == nullptr) {
    report.warnings.push_back("API key permissions not checked (no spot API host)");
    return;
  }
  KeyRestrictions k;
  std::string error;
  if (!core::ok(spot->key_restrictions(k, error))) {
    report.failures.push_back("apiRestrictions: " + error);
    return;
  }
  if (!k.enable_futures) {
    report.failures.push_back("the API key may not trade futures");
  }
  if (k.enable_withdrawals) {
    report.failures.push_back("the API key may withdraw; a trading key must not");
  }
  if (!k.ip_restrict) {
    (expect.allow_unrestricted_ip ? report.warnings : report.failures)
        .emplace_back("the API key has no IP whitelist");
  }
}

void check_clock(RestClient& futures, const StartupExpectations& expect, StartupReport& report) {
  std::string error;
  if (!core::ok(futures.sync_time(error))) {
    report.failures.push_back("time: " + error);
    return;
  }
  report.time_offset_ms = futures.time_offset_ms();
  if (std::llabs(report.time_offset_ms) > expect.max_time_offset_ms) {
    report.failures.push_back("the local clock is " + std::to_string(report.time_offset_ms) +
                              " ms off the venue's (at most " +
                              std::to_string(expect.max_time_offset_ms) + " allowed)");
  }
}

} // namespace

Status run_startup_checks(RestClient& futures, RestClient* spot, const StartupExpectations& expect,
                          StartupReport& report) {
  report = StartupReport{};
  // The clock first: the signed calls after it carry the venue's time.
  check_clock(futures, expect, report);
  if (spot != nullptr) {
    spot->set_time_offset(report.time_offset_ms); // one venue clock behind both hosts
  }
  check_instruments(futures, expect, report);
  check_account(futures, expect, report);
  check_key(spot, expect, report);
  return report.passed() ? Status::Ok : Status::InvalidState;
}

} // namespace jarvis::adapter::binance
