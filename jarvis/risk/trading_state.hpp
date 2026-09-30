#pragma once

#include <cstdint>
#include <string_view>

#include "jarvis/model/generated/enums.hpp"

// TradingState (docs/architecture.md section 10.2): which commands the risk gates let through,
// and who may move it. specs/tla/TradingState.tla is the source of truth for both tables below;
// tests/cpp/test_risk.cpp checks them against it.
//
// The effective state is the strictest of three parts:
//   base      set by the post-trade monitors, which only tighten it (Active -> Reducing ->
//             Halted), and by admin commands, the only way to loosen it;
//   sync      Halted while the node reconciles (start and every reconnect), cleared when it
//             is in sync again;
//   degraded  Reducing while the node is Degraded, cleared when it recovers.
// The holds clear themselves; nothing but an admin command undoes what a monitor did.

namespace jarvis::risk {

// What a command does to exposure, for the permission matrix.
enum class CommandKind : std::uint8_t {
  Open = 0,     // a new order that may increase the position
  Reduce = 1,   // a new order that only reduces the position
  Modify = 2,   // a modify that does not increase the order's quantity
  ModifyUp = 3, // a modify that increases it
  Cancel = 4,
};

[[nodiscard]] constexpr std::string_view to_string(CommandKind k) noexcept {
  switch (k) {
  case CommandKind::Open:
    return "OPEN";
  case CommandKind::Reduce:
    return "REDUCE";
  case CommandKind::Modify:
    return "MODIFY";
  case CommandKind::ModifyUp:
    return "MODIFY_UP";
  case CommandKind::Cancel:
    return "CANCEL";
  }
  return "";
}

// Spec names of the states (specs/tla/TradingState.tla).
[[nodiscard]] constexpr std::string_view spec_name(model::TradingState s) noexcept {
  switch (s) {
  case model::TradingState::Active:
    return "ACTIVE";
  case model::TradingState::Reducing:
    return "REDUCING";
  case model::TradingState::Halted:
    return "HALTED";
  }
  return "";
}

//           Open   Reduce  Modify  ModifyUp  Cancel
// Active    yes    yes     yes     yes       yes
// Reducing  no     yes     yes     no        yes
// Halted    no     no      no      no        yes
[[nodiscard]] constexpr bool allowed(model::TradingState state, CommandKind kind) noexcept {
  switch (state) {
  case model::TradingState::Active:
    return true;
  case model::TradingState::Reducing:
    return kind == CommandKind::Reduce || kind == CommandKind::Modify ||
           kind == CommandKind::Cancel;
  case model::TradingState::Halted:
    return kind == CommandKind::Cancel;
  }
  return false;
}

enum class TradingTrigger : std::uint8_t {
  SyncStarted = 0, // reconciliation begins
  Synced = 1,      // reconciliation finished
  Degraded = 2,    // the node lost health
  Recovered = 3,   // the node regained health
  SoftLimit = 4,   // a monitor: daily loss or drawdown past its limit, MARGIN_CALL
  HardLimit = 5,   // a monitor: past the hard limit (and the KillSwitch fires)
  AdminHalt = 6,
  AdminReduce = 7,
  AdminResume = 8,
};

[[nodiscard]] constexpr std::string_view to_string(TradingTrigger t) noexcept {
  switch (t) {
  case TradingTrigger::SyncStarted:
    return "SYNC_STARTED";
  case TradingTrigger::Synced:
    return "SYNCED";
  case TradingTrigger::Degraded:
    return "DEGRADED";
  case TradingTrigger::Recovered:
    return "RECOVERED";
  case TradingTrigger::SoftLimit:
    return "SOFT_LIMIT";
  case TradingTrigger::HardLimit:
    return "HARD_LIMIT";
  case TradingTrigger::AdminHalt:
    return "ADMIN_HALT";
  case TradingTrigger::AdminReduce:
    return "ADMIN_REDUCE";
  case TradingTrigger::AdminResume:
    return "ADMIN_RESUME";
  }
  return "";
}

namespace detail {
[[nodiscard]] constexpr int rank(model::TradingState s) noexcept {
  switch (s) {
  case model::TradingState::Active:
    return 0;
  case model::TradingState::Reducing:
    return 1;
  case model::TradingState::Halted:
    return 2;
  }
  return 2;
}
[[nodiscard]] constexpr model::TradingState stricter(model::TradingState a,
                                                     model::TradingState b) noexcept {
  return rank(a) >= rank(b) ? a : b;
}
} // namespace detail

class TradingStateMachine {
public:
  explicit constexpr TradingStateMachine(
      model::TradingState initial = model::TradingState::Active) noexcept
      : base_{initial} {}

  [[nodiscard]] constexpr model::TradingState state() const noexcept {
    model::TradingState s = base_;
    if (degraded_) {
      s = detail::stricter(s, model::TradingState::Reducing);
    }
    if (syncing_) {
      s = model::TradingState::Halted;
    }
    return s;
  }
  [[nodiscard]] constexpr model::TradingState base() const noexcept { return base_; }
  [[nodiscard]] constexpr bool syncing() const noexcept { return syncing_; }
  [[nodiscard]] constexpr bool degraded() const noexcept { return degraded_; }

  // Applies a trigger; returns whether the effective state changed.
  constexpr bool apply(TradingTrigger trigger) noexcept {
    const model::TradingState before = state();
    switch (trigger) {
    case TradingTrigger::SyncStarted:
      syncing_ = true;
      break;
    case TradingTrigger::Synced:
      syncing_ = false;
      break;
    case TradingTrigger::Degraded:
      degraded_ = true;
      break;
    case TradingTrigger::Recovered:
      degraded_ = false;
      break;
    case TradingTrigger::SoftLimit:
      base_ = detail::stricter(base_, model::TradingState::Reducing);
      break;
    case TradingTrigger::HardLimit:
    case TradingTrigger::AdminHalt:
      base_ = model::TradingState::Halted;
      break;
    case TradingTrigger::AdminReduce:
      base_ = model::TradingState::Reducing;
      break;
    case TradingTrigger::AdminResume:
      base_ = model::TradingState::Active;
      break;
    }
    return state() != before;
  }

  // Snapshot encoding (core/state.hpp).
  template <typename Ar> void state(Ar& ar) { ar(base_, syncing_, degraded_); }

private:
  model::TradingState base_;
  bool syncing_ = false;
  bool degraded_ = false;
};

} // namespace jarvis::risk
