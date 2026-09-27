// Python classes for market data, instruments, accounts, order, position and kernel events.
// Attribute names come from model::fields() (jarvis/model/schema.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "common.hpp"
#include "events.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/position_events.hpp"
#include "jarvis/node/event_text.hpp"

namespace jarvis::py {

core::Status validate_value(const m::Bar& bar) {
  m::Bar out;
  return m::Bar::create(bar.bar_type, bar.open, bar.high, bar.low, bar.close, bar.volume,
                        bar.ts_event, bar.ts_init, out);
}
core::Status validate_value(const m::CurrencyPair& instrument) {
  return m::validate(m::Instrument{instrument});
}
core::Status validate_value(const m::CryptoPerpetual& instrument) {
  return m::validate(m::Instrument{instrument});
}
core::Status validate_value(const m::CryptoFuture& instrument) {
  return m::validate(m::Instrument{instrument});
}
core::Status validate_value(const m::AccountBalance& balance) {
  m::AccountBalance out;
  return m::AccountBalance::create(balance.total, balance.locked, balance.free, out);
}

m::OrderBookDeltas PyOrderBookDeltas::view() const {
  m::OrderBookDeltas out;
  check(m::OrderBookDeltas::create(deltas, out), "OrderBookDeltas");
  return out;
}

namespace {

template <typename T> std::vector<std::byte> encode_payload(const T& value) {
  std::vector<std::byte> buffer(m::wire::kMaxPayload);
  m::wire::Writer w{buffer};
  m::wire::put_payload(w, value);
  if (!w.ok()) {
    throw nb::value_error("value too large to encode");
  }
  buffer.resize(w.size());
  return buffer;
}

std::string event_repr(const m::Event& event) {
  std::string out;
  node::append_event_text(out, event);
  return out;
}

nb::list list_of_deltas(const std::vector<m::OrderBookDelta>& deltas) {
  nb::list out;
  for (const m::OrderBookDelta& d : deltas) {
    out.append(nb::cast(d));
  }
  return out;
}

void bind_deltas(nb::module_& mod) {
  nb::class_<PyOrderBookDeltas>(mod, "OrderBookDeltas",
                                "A batch of order book deltas for one instrument; flags, sequence "
                                "and timestamps are those of the last delta.")
      .def(
          "__init__",
          [](PyOrderBookDeltas* self, nb::handle instrument_id, const nb::list& deltas) {
            PyOrderBookDeltas out;
            from_py(instrument_id, out.instrument_id, "instrument_id");
            for (nb::handle item : deltas) {
              m::OrderBookDelta d;
              from_py(item, d, "deltas");
              if (!(d.instrument_id == out.instrument_id)) {
                throw nb::value_error("OrderBookDeltas: every delta must be for instrument_id");
              }
              out.deltas.push_back(d);
            }
            static_cast<void>(out.view()); // validates: non-empty
            new (self) PyOrderBookDeltas{std::move(out)};
          },
          nb::arg("instrument_id"), nb::arg("deltas"))
      .def_prop_ro("instrument_id", [](const PyOrderBookDeltas& d) { return d.instrument_id; })
      .def_prop_ro("deltas", [](const PyOrderBookDeltas& d) { return list_of_deltas(d.deltas); })
      .def_prop_ro("flags", [](const PyOrderBookDeltas& d) { return d.view().flags; })
      .def_prop_ro("sequence", [](const PyOrderBookDeltas& d) { return d.view().sequence; })
      .def_prop_ro("ts_event", [](const PyOrderBookDeltas& d) { return d.view().ts_event.value(); })
      .def_prop_ro("ts_init", [](const PyOrderBookDeltas& d) { return d.view().ts_init.value(); })
      .def("__len__", [](const PyOrderBookDeltas& d) { return d.deltas.size(); })
      .def("__eq__",
           [](const PyOrderBookDeltas& a, nb::handle b) {
             if (!nb::isinstance<PyOrderBookDeltas>(b)) {
               return false;
             }
             return encode_payload(a.view()) ==
                    encode_payload(nb::cast<const PyOrderBookDeltas&>(b).view());
           })
      .def("__repr__", [](const PyOrderBookDeltas& d) { return event_repr(m::Event{d.view()}); });
}

void bind_account_state(nb::module_& mod) {
  nb::class_<PyAccountState>(mod, "AccountState", "Snapshot of an account's balances and margins.")
      .def(
          "__init__",
          [](PyAccountState* self, nb::handle account_id, m::AccountType account_type,
             nb::handle base_currency, const nb::list& balances, const nb::list& margins,
             bool is_reported, nb::handle event_id, nb::handle ts_event, nb::handle ts_init) {
            PyAccountState out;
            from_py(account_id, out.base.account_id, "account_id");
            out.base.account_type = account_type;
            from_py(base_currency, out.base.base_currency, "base_currency");
            for (nb::handle item : balances) {
              m::AccountBalance b;
              from_py(item, b, "balances");
              out.balances.push_back(b);
            }
            for (nb::handle item : margins) {
              m::MarginBalance mb;
              from_py(item, mb, "margins");
              out.margins.push_back(mb);
            }
            out.base.is_reported = is_reported;
            from_py(event_id, out.base.event_id, "event_id");
            from_py(ts_event, out.base.ts_event, "ts_event");
            from_py(ts_init, out.base.ts_init, "ts_init");
            new (self) PyAccountState{std::move(out)};
          },
          nb::arg("account_id"), nb::arg("account_type"), nb::arg("base_currency").none(),
          nb::arg("balances"), nb::arg("margins"), nb::arg("is_reported"), nb::arg("event_id"),
          nb::arg("ts_event"), nb::arg("ts_init"))
      .def_prop_ro("account_id", [](const PyAccountState& s) { return s.base.account_id; })
      .def_prop_ro("account_type", [](const PyAccountState& s) { return s.base.account_type; })
      .def_prop_ro("base_currency",
                   [](const PyAccountState& s) { return to_py(s.base.base_currency); })
      .def_prop_ro("balances", [](const PyAccountState& s) { return s.balances; })
      .def_prop_ro("margins", [](const PyAccountState& s) { return s.margins; })
      .def_prop_ro("is_reported", [](const PyAccountState& s) { return s.base.is_reported; })
      .def_prop_ro("event_id", [](const PyAccountState& s) { return s.base.event_id; })
      .def_prop_ro("ts_event", [](const PyAccountState& s) { return s.base.ts_event.value(); })
      .def_prop_ro("ts_init", [](const PyAccountState& s) { return s.base.ts_init.value(); })
      .def("__eq__",
           [](const PyAccountState& a, nb::handle b) {
             if (!nb::isinstance<PyAccountState>(b)) {
               return false;
             }
             return encode_payload(a.view()) ==
                    encode_payload(nb::cast<const PyAccountState&>(b).view());
           })
      .def("__repr__", [](const PyAccountState& s) { return event_repr(m::Event{s.view()}); });
}

template <typename E, std::size_t N>
void bind_kernel_enum(nb::module_& mod, const char* name, const std::array<E, N>& values) {
  nb::enum_<E> e(mod, name);
  for (const E v : values) {
    e.value(std::string{m::to_string(v)}.c_str(), v);
  }
}

void bind_kernel(nb::module_& mod) {
  bind_kernel_enum(mod, "NodeState",
                   std::array{m::NodeState::Init, m::NodeState::Wired, m::NodeState::Starting,
                              m::NodeState::Syncing, m::NodeState::Running, m::NodeState::Degraded,
                              m::NodeState::Stopping, m::NodeState::Stopped,
                              m::NodeState::Faulted});
  bind_kernel_enum(mod, "LifecycleReason",
                   std::array{m::LifecycleReason::Configured, m::LifecycleReason::RunRequested,
                              m::LifecycleReason::Started, m::LifecycleReason::Synced,
                              m::LifecycleReason::HealthLost, m::LifecycleReason::HealthRestored,
                              m::LifecycleReason::EndOfData, m::LifecycleReason::ShutdownRequested,
                              m::LifecycleReason::Drained, m::LifecycleReason::Fault});
  bind_kernel_enum(mod, "StrategyErrorKind",
                   std::array{m::StrategyErrorKind::Exception, m::StrategyErrorKind::Overrun});
  bind_kernel_enum(mod, "ShutdownMode",
                   std::array{m::ShutdownMode::CancelAllThenExit, m::ShutdownMode::ExitKeepOrders});
  bind_struct<m::TimerFired>(mod, "TimerFired", "A timer deadline reached (a recorded input).");
  bind_struct<m::BatchEnd>(mod, "BatchEnd", "End of one drained batch of inputs.");
  bind_struct<m::NodeLifecycle>(mod, "NodeLifecycle", "A node lifecycle transition.");
  bind_struct<m::StrategyError>(mod, "StrategyError", "A strategy callback failed or overran.");
  bind_struct<m::Shutdown>(mod, "Shutdown", "A shutdown request.");
}

template <typename T> void bind_instrument(nb::module_& mod, const char* name, const char* doc) {
  bind_struct<T>(mod, name, doc)
      .def(
          "notional_value",
          [](const T& self, nb::handle quantity, nb::handle price) {
            m::Quantity q;
            m::Price p;
            from_py(quantity, q, "quantity");
            from_py(price, p, "price");
            m::Money out;
            check(m::notional_value(self.common, q, p, out), "notional_value");
            return out;
          },
          nb::arg("quantity"), nb::arg("price"),
          "quantity * multiplier * price (inverse: / price), truncated to the currency grid.")
      .def_prop_ro("instrument_class",
                   [](const T& self) { return m::instrument_class(m::Instrument{self}); })
      .def_prop_ro("asset_class",
                   [](const T& self) { return m::asset_class(m::Instrument{self}); });
}

} // namespace

void bind_events(nb::module_& mod) {
  bind_struct<m::QuoteTick>(mod, "QuoteTick", "Best bid and ask.");
  bind_struct<m::TradeTick>(mod, "TradeTick", "One trade (Binance aggTrade).");
  bind_struct<m::Bar>(mod, "Bar", "OHLCV bar.");
  bind_struct<m::OrderBookDelta>(mod, "OrderBookDelta", "One order book change.");
  bind_deltas(mod);
  bind_struct<m::OrderBookDepth>(mod, "OrderBookDepth10", "Up to ten aggregated levels per side.");
  bind_struct<m::InstrumentStatus>(mod, "InstrumentStatus", "Trading status of an instrument.");
  bind_struct<m::MarkPriceUpdate>(mod, "MarkPriceUpdate", "Mark price.");
  bind_struct<m::IndexPriceUpdate>(mod, "IndexPriceUpdate", "Index price.");
  bind_struct<m::FundingRateUpdate>(mod, "FundingRateUpdate", "Funding rate of a perpetual.");
  bind_struct<m::InstrumentClose>(mod, "InstrumentClose", "Close price of an instrument.");
  bind_struct<m::LiquidationOrder>(mod, "LiquidationOrder",
                                   "A liquidation published by the venue (jarvis extension).");

  bind_instrument<m::CurrencyPair>(mod, "CurrencyPair", "Spot pair.");
  bind_instrument<m::CryptoPerpetual>(mod, "CryptoPerpetual", "Perpetual swap.");
  bind_instrument<m::CryptoFuture>(mod, "CryptoFuture", "Dated future.");
  bind_struct<m::AccountBalance>(mod, "AccountBalance", "total == locked + free, one currency.")
      .def_prop_ro("currency", [](const m::AccountBalance& b) { return b.total.currency(); });
  bind_struct<m::MarginBalance>(mod, "MarginBalance", "Initial and maintenance margin.");
  bind_account_state(mod);

  bind_struct<m::OrderInitialized>(mod, "OrderInitialized", "An order was created.");
  bind_struct<m::OrderDenied>(mod, "OrderDenied", "The risk gates denied an order.");
  bind_struct<m::OrderEmulated>(mod, "OrderEmulated", "An order is held for emulation.");
  bind_struct<m::OrderReleased>(mod, "OrderReleased", "An emulated order was released.");
  bind_struct<m::OrderSubmitted>(mod, "OrderSubmitted", "An order was sent to the venue.");
  bind_struct<m::OrderAccepted>(mod, "OrderAccepted", "The venue accepted an order.");
  bind_struct<m::OrderRejected>(mod, "OrderRejected", "The venue rejected an order.");
  bind_struct<m::OrderCanceled>(mod, "OrderCanceled", "An order was canceled.");
  bind_struct<m::OrderExpired>(mod, "OrderExpired", "An order expired.");
  bind_struct<m::OrderTriggered>(mod, "OrderTriggered", "A conditional order triggered.");
  bind_struct<m::OrderPendingUpdate>(mod, "OrderPendingUpdate", "A modify request is in flight.");
  bind_struct<m::OrderPendingCancel>(mod, "OrderPendingCancel", "A cancel request is in flight.");
  bind_struct<m::OrderModifyRejected>(mod, "OrderModifyRejected", "The venue refused a modify.");
  bind_struct<m::OrderCancelRejected>(mod, "OrderCancelRejected", "The venue refused a cancel.");
  bind_struct<m::OrderUpdated>(mod, "OrderUpdated", "An order was modified.");
  bind_struct<m::OrderFilled>(mod, "OrderFilled", "An order (partially) filled.");
  bind_struct<m::OrderFillVoided>(mod, "OrderFillVoided", "The venue voided an earlier fill.");

  bind_struct<m::PositionOpened>(mod, "PositionOpened", "A position opened.");
  bind_struct<m::PositionChanged>(mod, "PositionChanged", "A position changed.");
  bind_struct<m::PositionClosed>(mod, "PositionClosed", "A position closed.");
  bind_struct<m::PositionAdjusted>(mod, "PositionAdjusted", "A position was adjusted.");

  bind_kernel(mod);

  bind_struct<m::FeatureUpdate>(mod, "FeatureUpdate",
                                "A kernel feature value (an output record keyed by its input).");
  bind_struct<m::StrategyRecord>(mod, "StrategyRecord",
                                 "A value a strategy recorded with ctx.record(tag, value).");
  bind_struct<m::SubmitOrder>(mod, "SubmitOrder", "A new order sent to the venue (a command).");
  bind_struct<m::ModifyOrder>(mod, "ModifyOrder", "A LIMIT order's new quantity and price.");
  bind_struct<m::CancelOrder>(mod, "CancelOrder", "A cancel request for one order.");
  bind_struct<m::CancelAllOrders>(mod, "CancelAllOrders",
                                  "A cancel request for every open order of an instrument.");
}

nb::object output_to_py(const m::Output& output) {
  return std::visit([](const auto& o) -> nb::object { return nb::cast(o, nb::rv_policy::copy); },
                    output);
}

namespace {
template <std::size_t I = 0> bool output_from_py_impl(nb::handle object, m::Output& out) {
  if constexpr (I == std::variant_size_v<m::Output>) {
    return false;
  } else {
    using T = std::variant_alternative_t<I, m::Output>;
    // OrderDenied is also an input event: only an explicit output wrapper would be ambiguous,
    // so an OrderDenied object is written as the input kind (see EventLogWriter.append).
    if constexpr (!std::is_same_v<T, m::OrderDenied>) {
      if (nb::isinstance<T>(object)) {
        out = nb::cast<T>(object);
        return true;
      }
    }
    return output_from_py_impl<I + 1>(object, out);
  }
}
} // namespace

bool output_from_py(nb::handle object, m::Output& out) { return output_from_py_impl(object, out); }

nb::object event_to_py(const m::Event& event) {
  return std::visit(
      [](const auto& e) -> nb::object {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, m::OrderBookDeltas>) {
          PyOrderBookDeltas out;
          out.instrument_id = e.instrument_id;
          out.deltas.assign(e.deltas.begin(), e.deltas.end());
          return nb::cast(std::move(out));
        } else if constexpr (std::is_same_v<T, m::AccountState>) {
          PyAccountState out;
          out.base = e;
          out.base.balances = {};
          out.base.margins = {};
          out.balances.assign(e.balances.begin(), e.balances.end());
          out.margins.assign(e.margins.begin(), e.margins.end());
          return nb::cast(std::move(out));
        } else {
          return nb::cast(e, nb::rv_policy::copy);
        }
      },
      event);
}

namespace {

template <std::size_t I = 0>
bool event_from_py_impl(nb::handle object, nb::object& holder, m::Event& out) {
  if constexpr (I == std::variant_size_v<m::Event>) {
    return false;
  } else {
    using T = std::variant_alternative_t<I, m::Event>;
    if constexpr (std::is_same_v<T, m::OrderBookDeltas>) {
      if (nb::isinstance<PyOrderBookDeltas>(object)) {
        holder = nb::borrow(object);
        out = nb::cast<const PyOrderBookDeltas&>(object).view();
        return true;
      }
    } else if constexpr (std::is_same_v<T, m::AccountState>) {
      if (nb::isinstance<PyAccountState>(object)) {
        holder = nb::borrow(object);
        out = nb::cast<const PyAccountState&>(object).view();
        return true;
      }
    } else if (nb::isinstance<T>(object)) {
      out = nb::cast<T>(object);
      return true;
    }
    return event_from_py_impl<I + 1>(object, holder, out);
  }
}

} // namespace

m::Event event_from_py(nb::handle object, nb::object& holder) {
  m::Event out;
  if (!event_from_py_impl(object, holder, out)) {
    type_error("event", "an event (TradeTick, QuoteTick, OrderFilled, ...)", object);
  }
  return out;
}

} // namespace jarvis::py
