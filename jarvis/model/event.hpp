#pragma once

#include <cstdint>
#include <variant>

#include "jarvis/core/clock.hpp"
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
