#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/state.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/position_events.hpp"

// The strategy API (docs/architecture.md section 9.4). A C++ strategy is any class with some of
// the callbacks below; missing callbacks are skipped at compile time. There is no base class and
// no virtual function: StaticStrategySet calls them directly, DynamicStrategySet through a
// generated function table. Callbacks return core::Status (or void, meaning Ok); a failure
// becomes a StrategyError event.
//
//   on_start(ctx) / on_stop(ctx)                  node enters Running / Stopping
//   on_reconciled(ctx, ReconcileOutcome)          the account was reconciled with the venue (at
//                                                 start, before on_start, and after reconnects)
//   on_trade(ctx, TradeTick)                      trades
//   on_quote(ctx, QuoteTick)                      best bid and ask
//   on_book(ctx, BookView)                        the book after an update
//   on_book_deltas(ctx, OrderBookDeltas)          book deltas as received
//   on_bar(ctx, Bar)                              bars (external or aggregated)
//   on_mark_price / on_index_price / on_funding_rate / on_instrument_status /
//   on_instrument_close / on_liquidation          the matching data types
//   on_feature(ctx, FeatureId, Decimal value, UnixNanos ts)
//   on_trade_batch(ctx, TradeBatch) / on_quote_batch(ctx, QuoteBatch)   OnBatch cadence
//   on_order_event(ctx, OrderEvent)               every event of this strategy's orders, from the
//                                                 kernel (submitted, denied, pending) or the venue
//   on_position_event(ctx, PositionEvent)         this strategy's positions: opened, changed,
//                                                 closed, adjusted (funding)
//   on_timer(ctx, TimerKey, UnixNanos deadline)
//   on_error(ctx, StrategyError)                  this strategy failed
//
// A strategy that keeps state of its own describes it for EngineState snapshots (section 16.3)
// with one member template, used to save and to restore it:
//
//   template <typename Ar> void state(Ar& ar) { ar(count_, last_price_, open_ids_); }
//
// `ar` takes integers, enums, bool, the model's value types (Price, Quantity, InstrumentId,
// ClientOrderId, ...), std::optional, std::array, std::variant and core::FixedVector of those.
// Without it a snapshot still holds the kernel's state, but restoring one leaves the strategy as
// it was constructed, so a node recovers such strategies by replaying the log instead.

namespace jarvis::strategy {

class Context;

// The data a callback receives, by pointer; valid during the callback only.
using DataView =
    std::variant<const model::TradeTick*, const model::QuoteTick*, const data::BookView*,
                 const model::OrderBookDeltas*, const model::Bar*, const model::MarkPriceUpdate*,
                 const model::IndexPriceUpdate*, const model::FundingRateUpdate*,
                 const model::InstrumentStatus*, const model::InstrumentClose*,
                 const model::LiquidationOrder*, const model::FeatureUpdate*>;

// All updates of one OnBatch subscription within one batch.
struct TradeBatch {
  model::InstrumentId instrument_id;
  std::span<const model::TradeTick> trades;
};
struct QuoteBatch {
  model::InstrumentId instrument_id;
  std::span<const model::QuoteTick> quotes;
};
using BatchView = std::variant<const TradeBatch*, const QuoteBatch*>;

namespace detail {

template <typename F> core::Status status_of(F&& call) {
  if constexpr (std::is_void_v<decltype(call())>) {
    call();
    return core::Status::Ok;
  } else {
    return call();
  }
}

} // namespace detail

template <typename S> core::Status invoke_start(S& s, Context& ctx) {
  if constexpr (requires { s.on_start(ctx); }) {
    return detail::status_of([&] { return s.on_start(ctx); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S> core::Status invoke_stop(S& s, Context& ctx) {
  if constexpr (requires { s.on_stop(ctx); }) {
    return detail::status_of([&] { return s.on_stop(ctx); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S>
core::Status invoke_reconciled(S& s, Context& ctx, const model::ReconcileOutcome& outcome) {
  if constexpr (requires { s.on_reconciled(ctx, outcome); }) {
    return detail::status_of([&] { return s.on_reconciled(ctx, outcome); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S>
core::Status invoke_timer(S& s, Context& ctx, core::TimerKey key, core::UnixNanos deadline) {
  if constexpr (requires { s.on_timer(ctx, key, deadline); }) {
    return detail::status_of([&] { return s.on_timer(ctx, key, deadline); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S> core::Status invoke_error(S& s, Context& ctx, const model::StrategyError& e) {
  if constexpr (requires { s.on_error(ctx, e); }) {
    return detail::status_of([&] { return s.on_error(ctx, e); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S>
core::Status invoke_order_event(S& s, Context& ctx, const model::OrderEvent& e) {
  if constexpr (requires { s.on_order_event(ctx, e); }) {
    return detail::status_of([&] { return s.on_order_event(ctx, e); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S>
core::Status invoke_position_event(S& s, Context& ctx, const model::PositionEvent& e) {
  if constexpr (requires { s.on_position_event(ctx, e); }) {
    return detail::status_of([&] { return s.on_position_event(ctx, e); });
  } else {
    return core::Status::Ok;
  }
}

template <typename S> core::Status invoke_batch(S& s, Context& ctx, const BatchView& view) {
  return std::visit(
      [&](const auto* b) -> core::Status {
        using B = std::remove_cv_t<std::remove_pointer_t<decltype(b)>>;
        if constexpr (std::is_same_v<B, TradeBatch>) {
          if constexpr (requires { s.on_trade_batch(ctx, *b); }) {
            return detail::status_of([&] { return s.on_trade_batch(ctx, *b); });
          }
        } else if constexpr (requires { s.on_quote_batch(ctx, *b); }) {
          return detail::status_of([&] { return s.on_quote_batch(ctx, *b); });
        }
        return core::Status::Ok;
      },
      view);
}

namespace detail {

// One overload per DataView alternative: the callback for that type, or a no-op.
template <typename S> core::Status call_data(S& s, Context& ctx, const model::TradeTick& v) {
  if constexpr (requires { s.on_trade(ctx, v); }) {
    return status_of([&] { return s.on_trade(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::QuoteTick& v) {
  if constexpr (requires { s.on_quote(ctx, v); }) {
    return status_of([&] { return s.on_quote(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const data::BookView& v) {
  if constexpr (requires { s.on_book(ctx, v); }) {
    return status_of([&] { return s.on_book(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::OrderBookDeltas& v) {
  if constexpr (requires { s.on_book_deltas(ctx, v); }) {
    return status_of([&] { return s.on_book_deltas(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::Bar& v) {
  if constexpr (requires { s.on_bar(ctx, v); }) {
    return status_of([&] { return s.on_bar(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::MarkPriceUpdate& v) {
  if constexpr (requires { s.on_mark_price(ctx, v); }) {
    return status_of([&] { return s.on_mark_price(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::IndexPriceUpdate& v) {
  if constexpr (requires { s.on_index_price(ctx, v); }) {
    return status_of([&] { return s.on_index_price(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S>
core::Status call_data(S& s, Context& ctx, const model::FundingRateUpdate& v) {
  if constexpr (requires { s.on_funding_rate(ctx, v); }) {
    return status_of([&] { return s.on_funding_rate(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::InstrumentStatus& v) {
  if constexpr (requires { s.on_instrument_status(ctx, v); }) {
    return status_of([&] { return s.on_instrument_status(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::InstrumentClose& v) {
  if constexpr (requires { s.on_instrument_close(ctx, v); }) {
    return status_of([&] { return s.on_instrument_close(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::LiquidationOrder& v) {
  if constexpr (requires { s.on_liquidation(ctx, v); }) {
    return status_of([&] { return s.on_liquidation(ctx, v); });
  } else {
    return core::Status::Ok;
  }
}
template <typename S> core::Status call_data(S& s, Context& ctx, const model::FeatureUpdate& v) {
  if constexpr (requires { s.on_feature(ctx, v.feature_id, v.value, v.ts_init); }) {
    return status_of([&] { return s.on_feature(ctx, v.feature_id, v.value, v.ts_init); });
  } else {
    return core::Status::Ok;
  }
}

} // namespace detail

// Calls the callback that matches the data type, if the strategy has it.
template <typename S> core::Status invoke_data(S& s, Context& ctx, const DataView& view) {
  return std::visit([&](const auto* d) { return detail::call_data(s, ctx, *d); }, view);
}

// A C++ strategy: a movable class. Every callback is optional.
template <typename S>
concept Strategy = std::is_class_v<S> && std::move_constructible<S>;

template <typename S>
concept StatefulStrategy = requires(S& s, core::StateWriter& w, core::StateReader& r) {
  s.state(w);
  s.state(r);
};

template <typename S> constexpr bool has_state() noexcept { return StatefulStrategy<S>; }

template <typename S> core::Status invoke_save(S& s, core::StateWriter& w) {
  if constexpr (StatefulStrategy<S>) {
    s.state(w);
    return w.status();
  } else {
    return core::Status::Ok;
  }
}

template <typename S> core::Status invoke_load(S& s, core::StateReader& r) {
  if constexpr (StatefulStrategy<S>) {
    s.state(r);
    return r.status();
  } else {
    return core::Status::Ok;
  }
}

} // namespace jarvis::strategy
