#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/order_events.hpp"

namespace jarvis::model {

// Kernel-side event types that are not part of the nautilus model (docs/architecture.md
// section 5.1). They are recorded in the event log because they cannot be recomputed on replay.

enum class NodeState : std::uint8_t {
  Init = 0,
  Wired = 1,
  Starting = 2,
  Syncing = 3,
  Running = 4,
  Degraded = 5,
  Stopping = 6,
  Stopped = 7,
  Faulted = 8,
};

enum class LifecycleReason : std::uint8_t {
  Configured = 0,
  Started = 1,
  Synced = 2,
  HealthLost = 3,
  HealthRestored = 4,
  EndOfData = 5,
  ShutdownRequested = 6,
  Drained = 7,
  Fault = 8,
};

enum class StrategyErrorKind : std::uint8_t {
  Exception = 0,
  Overrun = 1,
};

enum class ShutdownMode : std::uint8_t {
  CancelAllThenExit = 0,
  ExitKeepOrders = 1,
};

[[nodiscard]] constexpr std::string_view to_string(NodeState v) noexcept {
  switch (v) {
  case NodeState::Init:
    return "INIT";
  case NodeState::Wired:
    return "WIRED";
  case NodeState::Starting:
    return "STARTING";
  case NodeState::Syncing:
    return "SYNCING";
  case NodeState::Running:
    return "RUNNING";
  case NodeState::Degraded:
    return "DEGRADED";
  case NodeState::Stopping:
    return "STOPPING";
  case NodeState::Stopped:
    return "STOPPED";
  case NodeState::Faulted:
    return "FAULTED";
  }
  return "";
}
[[nodiscard]] constexpr std::string_view to_string(LifecycleReason v) noexcept {
  switch (v) {
  case LifecycleReason::Configured:
    return "CONFIGURED";
  case LifecycleReason::Started:
    return "STARTED";
  case LifecycleReason::Synced:
    return "SYNCED";
  case LifecycleReason::HealthLost:
    return "HEALTH_LOST";
  case LifecycleReason::HealthRestored:
    return "HEALTH_RESTORED";
  case LifecycleReason::EndOfData:
    return "END_OF_DATA";
  case LifecycleReason::ShutdownRequested:
    return "SHUTDOWN_REQUESTED";
  case LifecycleReason::Drained:
    return "DRAINED";
  case LifecycleReason::Fault:
    return "FAULT";
  }
  return "";
}
[[nodiscard]] constexpr std::string_view to_string(StrategyErrorKind v) noexcept {
  switch (v) {
  case StrategyErrorKind::Exception:
    return "EXCEPTION";
  case StrategyErrorKind::Overrun:
    return "OVERRUN";
  }
  return "";
}
[[nodiscard]] constexpr std::string_view to_string(ShutdownMode v) noexcept {
  switch (v) {
  case ShutdownMode::CancelAllThenExit:
    return "CANCEL_ALL_THEN_EXIT";
  case ShutdownMode::ExitKeepOrders:
    return "EXIT_KEEP_ORDERS";
  }
  return "";
}

// Wire decoding helpers for the kernel enums above (the nautilus enums get theirs generated).
[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, NodeState& out) noexcept {
  if (v > static_cast<std::uint8_t>(NodeState::Faulted)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<NodeState>(v);
  return core::Status::Ok;
}
[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, LifecycleReason& out) noexcept {
  if (v > static_cast<std::uint8_t>(LifecycleReason::Fault)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<LifecycleReason>(v);
  return core::Status::Ok;
}
[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, StrategyErrorKind& out) noexcept {
  if (v > static_cast<std::uint8_t>(StrategyErrorKind::Overrun)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<StrategyErrorKind>(v);
  return core::Status::Ok;
}
[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, ShutdownMode& out) noexcept {
  if (v > static_cast<std::uint8_t>(ShutdownMode::ExitKeepOrders)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<ShutdownMode>(v);
  return core::Status::Ok;
}

struct TimerFired {
  core::TimerKey key;
  core::UnixNanos deadline;
  core::UnixNanos ts_init;
};

// Marks the end of one drained batch; `Conflated` subscriptions are cut at these boundaries.
struct BatchEnd {
  std::uint64_t batch = 0;
  core::UnixNanos ts_init;
};

struct NodeLifecycle {
  NodeState from = NodeState::Init;
  NodeState to = NodeState::Init;
  LifecycleReason reason = LifecycleReason::Configured;
  core::UnixNanos ts_init;
};

struct StrategyError {
  std::uint32_t strategy_index = 0;
  StrategyErrorKind kind = StrategyErrorKind::Exception;
  std::uint64_t message_hash = 0;
  core::UnixNanos ts_init;
};

struct Shutdown {
  ShutdownMode mode = ShutdownMode::CancelAllThenExit;
  core::UnixNanos ts_init;
};

// The closed set of kernel input events. Adding an alternative forces every visitor to handle
// it at compile time.
using Event =
    std::variant<TradeTick, QuoteTick, OrderBookDeltas, Bar, MarkPriceUpdate, IndexPriceUpdate,
                 FundingRateUpdate, InstrumentStatus, InstrumentClose, LiquidationOrder,
                 OrderInitialized, OrderDenied, OrderEmulated, OrderReleased, OrderSubmitted,
                 OrderAccepted, OrderRejected, OrderCanceled, OrderExpired, OrderTriggered,
                 OrderPendingUpdate, OrderPendingCancel, OrderModifyRejected, OrderCancelRejected,
                 OrderUpdated, OrderFilled, OrderFillVoided, AccountState, TimerFired, BatchEnd,
                 NodeLifecycle, StrategyError, Shutdown>;

} // namespace jarvis::model
