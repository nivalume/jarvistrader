#pragma once

#include <vector>

#include <nanobind/nanobind.h>

#include "jarvis/model/account.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"

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

// Python object for an event decoded from a log (copies borrowed parts into owners).
nanobind::object event_to_py(const model::Event& event);

// The model event behind a Python event object; spans point into `holder`, which must outlive
// the returned event. Raises TypeError for objects that are not events.
model::Event event_from_py(nanobind::handle object, nanobind::object& holder);

} // namespace jarvis::py
