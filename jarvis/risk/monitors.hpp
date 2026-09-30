#pragma once

#include <cstdint>
#include <optional>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/risk/trading_state.hpp"

// Post-trade monitors (docs/architecture.md section 10.5). They run after fills, mark price
// updates and funding, read the account's equity (wallet balance plus unrealized PnL) in one
// currency, and can only produce TradingState triggers; they never send orders themselves (the
// KillSwitch that follows a hard limit belongs to the engine).
//
//   daily loss   equity at the first observation of the UTC day minus equity now;
//   drawdown     highest equity seen minus equity now;
//   margin ratio maintenance margin over equity.

namespace jarvis::risk {

struct MonitorLimits {
  std::optional<std::int64_t> daily_loss_raw; // soft: Reducing
  std::optional<std::int64_t> daily_halt_raw; // hard: Halted and KillSwitch
  std::optional<std::int64_t> drawdown_raw;   // soft
  std::uint32_t margin_ratio_bps = 0;         // soft when maintenance / equity reaches it; 0 off
};

class LossMonitor {
public:
  static constexpr std::uint64_t kDayNs = 86'400'000'000'000ULL;

  LossMonitor() noexcept = default;
  explicit LossMonitor(const MonitorLimits& limits) noexcept : limits_{limits} {}

  // The strictest trigger the observation calls for, if any.
  [[nodiscard]] std::optional<TradingTrigger> observe(core::UnixNanos now, std::int64_t equity_raw,
                                                      std::int64_t maintenance_raw) noexcept {
    const std::uint64_t day = now.value() / kDayNs;
    if (!day_ || *day_ != day) {
      day_ = day;
      day_start_raw_ = equity_raw;
    }
    const std::int64_t peak =
        peak_raw_.value_or(equity_raw) > equity_raw ? peak_raw_.value_or(equity_raw) : equity_raw;
    peak_raw_ = peak;
    const std::int64_t loss = day_start_raw_ - equity_raw;
    const std::int64_t drawdown = peak - equity_raw;
    daily_loss_raw_ = loss;
    drawdown_raw_ = drawdown;
    if (limits_.daily_halt_raw && loss >= *limits_.daily_halt_raw) {
      return TradingTrigger::HardLimit;
    }
    const bool margin =
        limits_.margin_ratio_bps > 0 && maintenance_raw > 0 &&
        (equity_raw <= 0 || static_cast<core::i128>(maintenance_raw) * 10'000 >=
                                static_cast<core::i128>(equity_raw) * limits_.margin_ratio_bps);
    if ((limits_.daily_loss_raw && loss >= *limits_.daily_loss_raw) ||
        (limits_.drawdown_raw && drawdown >= *limits_.drawdown_raw) || margin) {
      return TradingTrigger::SoftLimit;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::int64_t daily_loss_raw() const noexcept { return daily_loss_raw_; }
  [[nodiscard]] std::int64_t drawdown_raw() const noexcept { return drawdown_raw_; }
  [[nodiscard]] const MonitorLimits& limits() const noexcept { return limits_; }

  // Snapshot encoding (core/state.hpp); the limits come from the configuration.
  template <typename Ar> void state(Ar& ar) {
    ar(day_, day_start_raw_, peak_raw_, daily_loss_raw_, drawdown_raw_);
  }

private:
  MonitorLimits limits_;
  std::optional<std::uint64_t> day_;
  std::int64_t day_start_raw_ = 0;
  std::optional<std::int64_t> peak_raw_;
  std::int64_t daily_loss_raw_ = 0;
  std::int64_t drawdown_raw_ = 0;
};

} // namespace jarvis::risk
