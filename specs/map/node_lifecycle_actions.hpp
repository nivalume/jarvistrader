#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/event.hpp"

// NodeLifecycle's actions and the driver inputs they stand for (docs/architecture.md 18.2).
// The trace driver replays them through backtest::Driver::run_realtime over an Engine, with a
// pump that stages one action each time the driver goes idle:
//
//   Link(k, up)       ConnectionStatus(k, up) for MARKET_DATA or ORDER_ENTRY
//   Stream(up)        ConnectionStatus(USER_STREAM, up)
//   Snapshot          a VenueSnapshot listing the open orders as accepted
//   Stale             the clock jumps past node.market_data_stale_ms: the freshness timer fires
//   Fresh             a TradeTick (market data)
//   Submit            a ParamUpdate the strategy answers with a limit order
//   Answer            OrderCanceled for the oldest open order
//   Shutdown(m)       the pump asks to stop; DriverOptions::shutdown is m (none for NONE)
//   Timeout           the clock jumps past the drain time
//   Fault(PUMP)       the pump fails; Fault(LOG) the recorder fails from the next input on
//
// After each action the driver's state must match the spec's: the lifecycle (the kernel's node
// state, or the run summary's once the run ended), the account's reconciliation phase, the
// connection links, staleness, open orders, the recorded Shutdown mode, whether cancels went
// out, and the strategy's on_start and on_stop calls.

namespace jarvis::specmap::node_lifecycle {

inline constexpr std::array<std::string_view, 10> kActions = {
    "Link",   "Stream", "Snapshot", "Stale",   "Fresh",
    "Submit", "Answer", "Shutdown", "Timeout", "Fault"};

inline constexpr std::array<std::string_view, 3> kLinks = {"UNKNOWN", "UP", "DOWN"};

[[nodiscard]] constexpr std::string_view link_name(execution::LinkState s) noexcept {
  return kLinks[static_cast<std::size_t>(s)];
}

[[nodiscard]] constexpr bool names_match() {
  return link_name(execution::LinkState::Unknown) == "UNKNOWN" &&
         link_name(execution::LinkState::Up) == "UP" &&
         link_name(execution::LinkState::Down) == "DOWN" &&
         model::to_string(model::ConnectionKind::MarketData) == "MARKET_DATA" &&
         model::to_string(model::ConnectionKind::OrderEntry) == "ORDER_ENTRY" &&
         model::to_string(model::ShutdownMode::CancelAllThenExit) == "CANCEL_ALL_THEN_EXIT" &&
         model::to_string(model::ShutdownMode::ExitKeepOrders) == "EXIT_KEEP_ORDERS" &&
         execution::to_string(execution::SyncPhase::Local) == "LOCAL" &&
         execution::to_string(execution::SyncPhase::Synced) == "SYNCED";
}
static_assert(names_match(), "specs/map names must follow the model and execution enums");

} // namespace jarvis::specmap::node_lifecycle
