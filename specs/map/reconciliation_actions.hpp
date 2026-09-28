#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/generated/enums.hpp"

// Reconciliation's actions and the kernel inputs they stand for (docs/architecture.md 18.2).
// The trace driver replays them through the engine, whose reconciler is
// execution::Reconciler, and applies the driver's sync gate (engine/sync_gate.hpp) after every
// input, as backtest::Driver does with DriverOptions::await_sync:
//
//   Open(o), Fill(o), Cancel(o)   the venue; the driver keeps a copy to answer snapshots
//   Disconnect / Connect          ConnectionStatus(UserStream, down / up)
//   Deliver(o, n, st, f, t, k)    the message as a venue order event at venue time t: a fill of
//                                 trade <<o, n>> when n > 0, else OrderAccepted (st = "open") or
//                                 OrderCanceled (st = "done")
//   RequestSnapshot               the adapter's; no kernel input
//   SnapshotTaken(ts, st, f)      the driver's copy of the venue as a VenueSnapshot at T_s = ts
//   Reconcile                     that VenueSnapshot
//
// Spec order o is the strategy's o-th order (quantity MaxFill units). The spec's `lst` is the
// order's status (SUBMITTED "none", open statuses "open", closed ones "done"), `lf` its filled
// units, `lpos` the strategy's position in units (and the venue position), `seen` the trades
// the strategy received, `trading` the TradingState. The spec's Snapshotting is the adapter's:
// the kernel stays Buffering until the snapshot arrives.

namespace jarvis::specmap::reconciliation {

inline constexpr std::array<std::string_view, 9> kActions = {
    "Open",    "Fill",      "Cancel",          "Disconnect",   "Connect",
    "Deliver", "Reconcile", "RequestSnapshot", "SnapshotTaken"};

[[nodiscard]] constexpr std::string_view phase_name(execution::SyncPhase p) noexcept {
  switch (p) {
  case execution::SyncPhase::Local:
    return "Local";
  case execution::SyncPhase::Disconnected:
    return "Disconnected";
  case execution::SyncPhase::Buffering:
    return "Buffering";
  case execution::SyncPhase::Synced:
    return "Synced";
  }
  return "";
}

// The spec's phase as the kernel sees it.
[[nodiscard]] constexpr std::string_view kernel_phase(std::string_view spec_phase) noexcept {
  return spec_phase == "Snapshotting" ? std::string_view{"Buffering"} : spec_phase;
}

[[nodiscard]] constexpr std::string_view status_name(model::OrderStatus s) noexcept {
  if (s == model::OrderStatus::Submitted || s == model::OrderStatus::Initialized) {
    return "none";
  }
  return execution::is_open(s) ? "open" : "done";
}

} // namespace jarvis::specmap::reconciliation
