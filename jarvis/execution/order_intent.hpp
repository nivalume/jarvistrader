#pragma once

#include <optional>

#include "jarvis/core/time.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"

// What a strategy asks for (docs/architecture.md section 9.4). The kernel assigns the
// ClientOrderId, runs both risk gates and turns the intent into an order and a command.
// Lives in the execution layer so the risk layer can check intents.

namespace jarvis::execution {

struct OrderIntent {
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::Buy;
  model::OrderType type = model::OrderType::Limit;
  model::Quantity quantity;
  std::optional<model::Price> price; // LIMIT only
  model::TimeInForce time_in_force = model::TimeInForce::Gtc;
  bool post_only = false;                     // Binance GTX
  bool reduce_only = false;                   // one-way mode only
  std::optional<core::UnixNanos> expire_time; // GTD only

  [[nodiscard]] static OrderIntent limit(const model::InstrumentId& id, model::OrderSide side,
                                         model::Quantity quantity, model::Price price,
                                         model::TimeInForce tif = model::TimeInForce::Gtc,
                                         bool post_only = false,
                                         bool reduce_only = false) noexcept {
    OrderIntent i;
    i.instrument_id = id;
    i.side = side;
    i.type = model::OrderType::Limit;
    i.quantity = quantity;
    i.price = price;
    i.time_in_force = tif;
    i.post_only = post_only;
    i.reduce_only = reduce_only;
    return i;
  }

  [[nodiscard]] static OrderIntent market(const model::InstrumentId& id, model::OrderSide side,
                                          model::Quantity quantity,
                                          bool reduce_only = false) noexcept {
    OrderIntent i;
    i.instrument_id = id;
    i.side = side;
    i.type = model::OrderType::Market;
    i.quantity = quantity;
    i.time_in_force = model::TimeInForce::Ioc;
    i.reduce_only = reduce_only;
    return i;
  }
};

} // namespace jarvis::execution
