#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/core/status.hpp"

// The checks a live node passes before it trades (docs/architecture.md section 14.6). They only
// read: jarvis never changes account settings, so a mismatch is for the operator to fix.
//
//   1. exchangeInfo: every configured symbol is a perpetual; one not TRADING is a warning.
//   2. positionSide/dual matches the account mode (section 8.5).
//   3. multiAssetsMargin matches the configuration.
//   4. positionRisk: each symbol's margin type, and its leverage when one is configured.
//   5. The API key may trade futures and may not withdraw; without an IP whitelist it fails
//      unless allowed. The permissions live on the spot API host; without a client for it (the
//      testnet has none) the check is skipped with a warning.
//   6. The local clock is within the allowed offset of the venue's; the offset is kept for
//      signing either way.

namespace jarvis::adapter::binance {

struct StartupExpectations {
  std::vector<std::string> symbols;
  bool hedge_mode = false;
  bool multi_assets = false;
  std::string margin_type = "cross"; // "cross" or "isolated", as positionRisk reports it
  std::uint32_t leverage = 0;        // 0: not checked
  std::int64_t max_time_offset_ms = 1000;
  bool allow_unrestricted_ip = false;
};

struct StartupReport {
  std::vector<std::string> failures;
  std::vector<std::string> warnings;
  std::vector<PerpetualDefinition> instruments;
  std::int64_t time_offset_ms = 0;

  [[nodiscard]] bool passed() const noexcept { return failures.empty(); }
};

// Runs every check even after one fails, so that the report lists all problems at once. Ok when
// they all passed, InvalidState otherwise.
[[nodiscard]] core::Status run_startup_checks(RestClient& futures, RestClient* spot,
                                              const StartupExpectations& expect,
                                              StartupReport& report);

} // namespace jarvis::adapter::binance
