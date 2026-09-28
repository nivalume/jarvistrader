#pragma once

#include <vector>

#include <nanobind/nanobind.h>

#include "jarvis/model/account.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"

namespace jarvis::py {

// Python-side owners of the variable-length events; the model structs only borrow their parts.
struct PyOrderBookDeltas {
  model::InstrumentId instrument_id;
  std::vector<model::OrderBookDelta> deltas;

  [[nodiscard]] model::OrderBookDeltas view() const;
};

struct PyAccountState {
  model::AccountState base; // balances and margins are empty here
  std::vector<model::AccountBalance> balances;
  std::vector<model::MarginBalance> margins;

  [[nodiscard]] model::AccountState view() const {
    model::AccountState s = base;
    s.balances = balances;
    s.margins = margins;
    return s;
  }
};

struct PyVenueSnapshot {
  model::VenueSnapshot base; // the lists are empty here
  std::vector<model::AccountBalance> balances;
  std::vector<model::OrderStatusReport> orders;
  std::vector<model::FillReport> fills;
  std::vector<model::PositionStatusReport> positions;

  [[nodiscard]] model::VenueSnapshot view() const {
    model::VenueSnapshot s = base;
    s.balances = balances;
    s.orders = orders;
    s.fills = fills;
    s.positions = positions;
    return s;
  }
};

// Python object for an event decoded from a log (copies borrowed parts into owners).
nanobind::object event_to_py(const model::Event& event);

// The model event behind a Python event object; spans point into `holder`, which must outlive
// the returned event. Raises TypeError for objects that are not events.
model::Event event_from_py(nanobind::handle object, nanobind::object& holder);

// Kernel outputs (FeatureUpdate, StrategyRecord) as Python objects and back.
nanobind::object output_to_py(const model::Output& output);
// True and sets `out` when `object` is a kernel output; false for anything else.
bool output_from_py(nanobind::handle object, model::Output& out);

} // namespace jarvis::py
