#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "jarvis/data/subscription.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/identifiers.hpp"

// Routing (docs/architecture.md section 7.2): which subscription row and kind an input event is
// delivered under. Instrument data is keyed by instrument id (the engine interns it to a slot);
// bars by bar type (interned to a bar key). Events that are not market data have no route.

namespace jarvis::data {

struct Route {
  DataKind kind = DataKind::Trade;
  const model::InstrumentId* instrument = nullptr; // for instrument-keyed kinds
  const model::BarType* bar_type = nullptr;        // for DataKind::Bar
};

// Not noexcept: std::visit may throw bad_variant_access, which cannot happen for Event.
[[nodiscard]] inline std::optional<Route> route_of(const model::Event& event) {
  return std::visit(
      [](const auto& e) -> std::optional<Route> {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, model::TradeTick>) {
          return Route{DataKind::Trade, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::QuoteTick>) {
          return Route{DataKind::Quote, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::OrderBookDeltas>) {
          return Route{DataKind::BookDeltas, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::Bar>) {
          return Route{DataKind::Bar, &e.bar_type.instrument_id, &e.bar_type};
        } else if constexpr (std::is_same_v<T, model::MarkPriceUpdate>) {
          return Route{DataKind::MarkPrice, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::IndexPriceUpdate>) {
          return Route{DataKind::IndexPrice, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::FundingRateUpdate>) {
          return Route{DataKind::FundingRate, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::InstrumentStatus>) {
          return Route{DataKind::Status, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::InstrumentClose>) {
          return Route{DataKind::Close, &e.instrument_id, nullptr};
        } else if constexpr (std::is_same_v<T, model::LiquidationOrder>) {
          return Route{DataKind::Liquidation, &e.instrument_id, nullptr};
        } else {
          return std::nullopt;
        }
      },
      event);
}

} // namespace jarvis::data
