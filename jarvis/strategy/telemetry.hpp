#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Kernel log records (docs/architecture.md section 19.2): what the kernel decided inside a step
// that no input or output shows, as an integer code and integer arguments. The shell adds the
// step's seq and ts, formats them as JSON lines and counts them. They are not written to the run
// log and change no state, so a replay produces the same records; a step's records are dropped
// when the next step starts.

namespace jarvis::strategy {

enum class LogCode : std::uint8_t {
  // args: from, to (model::TradingState), base (model::TradingState), holds (1: syncing,
  // 2: degraded)
  TradingStateChanged = 1,
  // strategy: the strategy; args: halt_node (0 or 1). Its open orders are canceled.
  StrategyHalted = 2,
  // args: cancels issued. Every order of every strategy (a hard loss limit, an admin
  // cancel_all, the shutdown).
  KillSwitch = 3,
  // args: nanoseconds since the last market data input, node.market_data_stale_ms in ns.
  MarketDataStale = 4,
};

inline constexpr std::uint16_t kNoLogStrategy = 0xFFFF;
inline constexpr std::size_t kLogsPerStep = 64; // more in one step are counted and dropped

struct LogRecord {
  LogCode code = LogCode::TradingStateChanged;
  std::uint16_t strategy = kNoLogStrategy;
  std::array<std::int64_t, 4> args{};
};

[[nodiscard]] constexpr std::string_view to_string(LogCode c) noexcept {
  switch (c) {
  case LogCode::TradingStateChanged:
    return "trading_state";
  case LogCode::StrategyHalted:
    return "strategy_halted";
  case LogCode::KillSwitch:
    return "kill_switch";
  case LogCode::MarketDataStale:
    return "market_data_stale";
  }
  return "unknown";
}

} // namespace jarvis::strategy
