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
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/reports.hpp"

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

// Why the node changed state; each reason drives the transitions listed in
// jarvis/engine/lifecycle.hpp.
enum class LifecycleReason : std::uint8_t {
  Configured = 0,        // Init -> Wired
  RunRequested = 1,      // Wired -> Starting
  Started = 2,           // Starting -> Syncing: log open, connections up
  Synced = 3,            // Syncing -> Running
  HealthLost = 4,        // Syncing | Running -> Degraded
  HealthRestored = 5,    // Degraded -> Syncing
  EndOfData = 6,         // Running -> Stopping (backtest)
  ShutdownRequested = 7, // Wired | Starting | Syncing | Running | Degraded -> Stopping
  Drained = 8,           // Stopping -> Stopped
  Fault = 9,             // any non-terminal state -> Faulted
};

enum class StrategyErrorKind : std::uint8_t {
  Exception = 0,
  Overrun = 1,
};

enum class ShutdownMode : std::uint8_t {
  CancelAllThenExit = 0,
  ExitKeepOrders = 1,
};

// What an operator's admin command asks for (docs/architecture.md section 19.3).
enum class AdminAction : std::uint8_t {
  Halt = 0,      // TradingState Halted: nothing new, cancels only
  Reduce = 1,    // TradingState Reducing
  Resume = 2,    // TradingState back to Active (what monitors or light checks lowered)
  CancelAll = 3, // cancel every open order of every strategy
  Shutdown = 4,  // stop the node ([node] shutdown decides how)
};

// Which connection a ConnectionStatus reports (docs/architecture.md section 4.4).
enum class ConnectionKind : std::uint8_t {
  MarketData = 0, // the market data streams
  UserStream = 1, // the user data stream (orders, fills, account)
  OrderEntry = 2, // the order channel (WebSocket API, REST fallback)
};

// Which venue limit a RateLimitFeedback reports (docs/architecture.md section 10.4).
enum class RateLimitKind : std::uint8_t {
  Orders = 0,        // the account's order count (X-MBX-ORDER-COUNT-*, rateLimits ORDERS)
  RequestWeight = 1, // the IP's request weight (X-MBX-USED-WEIGHT-*, rateLimits REQUEST_WEIGHT)
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
  case LifecycleReason::RunRequested:
    return "RUN_REQUESTED";
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
[[nodiscard]] constexpr std::string_view to_string(AdminAction v) noexcept {
  switch (v) {
  case AdminAction::Halt:
    return "HALT";
  case AdminAction::Reduce:
    return "REDUCE";
  case AdminAction::Resume:
    return "RESUME";
  case AdminAction::CancelAll:
    return "CANCEL_ALL";
  case AdminAction::Shutdown:
    return "SHUTDOWN";
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

[[nodiscard]] constexpr std::string_view to_string(RateLimitKind v) noexcept {
  switch (v) {
  case RateLimitKind::Orders:
    return "ORDERS";
  case RateLimitKind::RequestWeight:
    return "REQUEST_WEIGHT";
  }
  return "?";
}

[[nodiscard]] constexpr std::string_view to_string(ConnectionKind v) noexcept {
  switch (v) {
  case ConnectionKind::MarketData:
    return "MARKET_DATA";
  case ConnectionKind::UserStream:
    return "USER_STREAM";
  case ConnectionKind::OrderEntry:
    return "ORDER_ENTRY";
  }
  return "?";
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
[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, AdminAction& out) noexcept {
  if (v > static_cast<std::uint8_t>(AdminAction::Shutdown)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<AdminAction>(v);
  return core::Status::Ok;
}
[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, ShutdownMode& out) noexcept {
  if (v > static_cast<std::uint8_t>(ShutdownMode::ExitKeepOrders)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<ShutdownMode>(v);
  return core::Status::Ok;
}

[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, RateLimitKind& out) noexcept {
  if (v > static_cast<std::uint8_t>(RateLimitKind::RequestWeight)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<RateLimitKind>(v);
  return core::Status::Ok;
}

[[nodiscard]] constexpr core::Status from_value(std::uint8_t v, ConnectionKind& out) noexcept {
  if (v > static_cast<std::uint8_t>(ConnectionKind::OrderEntry)) {
    return core::Status::OutOfRange;
  }
  out = static_cast<ConnectionKind>(v);
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

// An operator's command from the admin socket, recorded like any input so it replays.
struct AdminCommand {
  AdminAction action = AdminAction::Halt;
  core::UnixNanos ts_init;
};

// The first input of a live run (docs/architecture.md sections 4.1 and 16.3): new ClientOrderIds
// take this run's `epoch`, so a replay issues the ids the run issued. With prior_seq > 0 the run
// continues an earlier run of this node whose state the kernel was restored to, up to that run's
// input `prior_seq`, and what belonged to the earlier process is reset: its venue connections are
// gone (the account reconciles again), shutdown and stop requests clear, the lifecycle starts
// over from Init, and the countdownCancelAll timer is disarmed.
struct RunStart {
  std::uint64_t epoch = 0;
  std::uint64_t prior_seq = 0;
  core::UnixNanos ts_init;
};

// The venue's count of one rate limit window (docs/architecture.md section 10.4), from response
// headers and the WebSocket API's rateLimits: the kernel raises its own count of the matching
// order window to at least `used`. An HTTP 429 arrives as used = limit (the window is spent).
struct RateLimitFeedback {
  RateLimitKind kind = RateLimitKind::Orders;
  std::uint64_t interval_ns = 0; // the window's length (10 s, 1 min)
  std::uint32_t used = 0;
  std::uint32_t limit = 0; // the venue's limit; 0 when not reported
  core::UnixNanos ts_init;
};

// A venue connection went down or came back (the Health* events of docs/architecture.md section
// 4.4). The user stream going down sends the account's session back to the start of
// reconciliation (section 15.3); the node is Degraded until it is synced again.
struct ConnectionStatus {
  Venue venue;
  ConnectionKind kind = ConnectionKind::UserStream;
  bool up = false;
  core::UnixNanos ts_init;
};

// The closed set of kernel input events. Adding an alternative forces every visitor to handle
// it at compile time. Instrument definitions (exchangeInfo, catalog files) are inputs too, so a
// replay sees the same tick sizes, filters and margins the run saw.
using Event =
    std::variant<TradeTick, QuoteTick, OrderBookDeltas, Bar, MarkPriceUpdate, IndexPriceUpdate,
                 FundingRateUpdate, InstrumentStatus, InstrumentClose, LiquidationOrder,
                 OrderInitialized, OrderDenied, OrderEmulated, OrderReleased, OrderSubmitted,
                 OrderAccepted, OrderRejected, OrderCanceled, OrderExpired, OrderTriggered,
                 OrderPendingUpdate, OrderPendingCancel, OrderModifyRejected, OrderCancelRejected,
                 OrderUpdated, OrderFilled, OrderFillVoided, AccountState, TimerFired, BatchEnd,
                 NodeLifecycle, StrategyError, Shutdown, CurrencyPair, CryptoPerpetual,
                 CryptoFuture, RateLimitFeedback, ConnectionStatus, VenueSnapshot, AdminCommand,
                 RunStart>;

// ts_init of any input event (order events keep it in their header). Not noexcept: std::visit
// may throw bad_variant_access, which cannot happen for these types.
[[nodiscard]] constexpr core::UnixNanos ts_init_of(const Event& event) {
  return std::visit(
      [](const auto& e) -> core::UnixNanos {
        if constexpr (requires { e.ts_init; }) {
          return e.ts_init;
        } else if constexpr (requires { e.common.ts_init; }) {
          return e.common.ts_init; // instrument definitions
        } else {
          return e.header.ts_init;
        }
      },
      event);
}

} // namespace jarvis::model
