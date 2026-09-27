#pragma once

#include <array>
#include <string_view>

#include "jarvis/adapter/binance/depth_sync.hpp"

// DepthSync's actions and the synchronizer inputs they stand for (docs/architecture.md 18.2).
// The trace driver replays them through adapter::binance::DepthSync:
//
//   Update, Publish, Lose, Serve   the exchange and the network; no input to the client
//   Connect                        DepthSync::connected
//   Disconnect                     DepthSync::disconnected
//   Request                        DepthSync::snapshot_requested
//   Receive(U, u, pu, ch)          DepthSync::on_diff with bid levels ch (size 0 deletes)
//   Arrive(L, book)                DepthSync::on_snapshot with the bid levels of book
//
// A spec price p is the bid p ticks above a base price and a spec size q is q lots. The spec's
// `local` is the synchronizer's bid book; `visible` and `out` are what the kernel holds after
// the OrderBookDeltas emitted so far.

namespace jarvis::specmap::depth_sync {

inline constexpr std::array<std::string_view, 9> kActions = {
    "Update", "Publish", "Lose", "Serve", "Connect", "Disconnect", "Request", "Receive", "Arrive"};

inline constexpr std::array<std::string_view, 5> kPhases = {"Idle", "Buffering", "Requested",
                                                            "Validating", "Synced"};

[[nodiscard]] constexpr std::string_view phase_name(adapter::binance::DepthSyncPhase p) noexcept {
  return kPhases[static_cast<std::size_t>(p)];
}

} // namespace jarvis::specmap::depth_sync
