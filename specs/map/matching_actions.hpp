#pragma once

#include <array>
#include <string_view>

#include "jarvis/backtest/matching/sim_exchange.hpp"

// Matching's actions and the simulator inputs they stand for (docs/architecture.md 18.2). The
// trace driver replays them through backtest::SimulatedExchange with FillModel::QueuePosition,
// one buy order at price p on a perpetual:
//
//   Rest        SubmitOrder, GTC limit buy at p; the best ask is above p, so it rests
//   Take(v)     QuoteTick offering v at p, the SubmitOrder, then the ask moves back above p
//   TradeAt(s)  TradeTick at p for s, the seller the aggressor
//   Through(s)  TradeTick one tick below p for s, the seller the aggressor
//   Level(n)    QuoteTick with the best bid at p for n
//   Gone        QuoteTick with the best bid one tick below p
//   Cross(v)    QuoteTick offering v at p while the order rests, then the ask moves back
//
// One spec unit is one lot of the order's size precision, so the spec's integer division is the
// simulator's (shrinking the queue ahead rounds down to the lot). The trace driver uses size
// precision 9, where a lot is one raw unit.

namespace jarvis::specmap::matching {

inline constexpr std::array<std::string_view, 7> kActions = {"Rest",  "Take", "TradeAt", "Through",
                                                             "Level", "Gone", "Cross"};

// The spec models the queue-position fill model only.
inline constexpr backtest::FillModel kFillModel = backtest::FillModel::QueuePosition;

} // namespace jarvis::specmap::matching
