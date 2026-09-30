#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/state.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/data/features.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/reports.hpp"
#include "jarvis/model/state_io.hpp"
#include "jarvis/portfolio/portfolio.hpp"
#include "jarvis/risk/trading_state.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/strategy/telemetry.hpp"

// The deterministic kernel (docs/architecture.md sections 3, 4.3 and 5): `step(key, event)`
// updates the kernel state, calls strategies and collects outputs. It reads nothing but its
// arguments and its state, so replaying the same inputs reproduces the same calls and outputs.
//
// Delivery order within a step is fixed: a trade goes to its subscribers first, then the
// features it updates, then the bars it completes; within one (row, kind) subscribers are served
// in subscription order. Subscribers are collected before any callback runs, so a callback that
// subscribes or unsubscribes changes the next delivery, not the current one.
//
// Order events: a venue event is applied to the OMS (a fill is also booked in the portfolio) and
// delivered to the order's strategy at once; position events and the events the kernel produces
// for commands (OrderSubmitted, OrderDenied, pending update and cancel) are delivered after the
// input's other callbacks, in the order they arose, including those arising from these
// deliveries.
//
// Reconciliation (execution/reconciliation.hpp): the user data stream's ConnectionStatus and
// VenueSnapshot drive the account's session. While it is not synced, venue order events and
// account states are held instead of applied; the snapshot sets the local state through
// synthesized venue events, each delivered like a venue event with its position events right
// after it, then the held events newer than the snapshot are applied.

namespace jarvis::engine {

namespace detail {

// Inputs whose arrival makes market data fresh (section 19.3).
template <typename T>
inline constexpr bool kMarketData =
    std::is_same_v<T, model::TradeTick> || std::is_same_v<T, model::QuoteTick> ||
    std::is_same_v<T, model::OrderBookDeltas> || std::is_same_v<T, model::Bar> ||
    std::is_same_v<T, model::MarkPriceUpdate> || std::is_same_v<T, model::IndexPriceUpdate> ||
    std::is_same_v<T, model::FundingRateUpdate> || std::is_same_v<T, model::LiquidationOrder>;

} // namespace detail

using strategy::KernelConfig;
using strategy::KernelServices;
using strategy::StrategyIndex;

// Where inputs come from (ReplaySource, the live rings).
template <typename S>
concept EventSource = requires(S& source, core::EventKey& key, model::Event& event) {
  { source.next(key, event) } -> std::same_as<core::Status>;
};

// Where outputs go (the event log, the order sender from M3).
namespace detail {
template <typename T, typename V> struct is_alternative : std::false_type {};
template <typename T, typename... A>
struct is_alternative<T, std::variant<A...>> : std::bool_constant<(std::is_same_v<T, A> || ...)> {};
template <typename T, typename V>
inline constexpr bool is_alternative_v = is_alternative<T, V>::value;
} // namespace detail

template <typename S>
concept CommandSink = requires(S& sink, const core::EventKey& key, const model::Output& output) {
  { sink.emit(key, output) } -> std::same_as<core::Status>;
};

template <strategy::StrategySet SS> class Engine {
public:
  Engine(const KernelConfig& config, SS& strategies,
         strategy::ErrorPolicy policy = strategy::ErrorPolicy::HaltStrategy)
      : k_{config}, ss_{&strategies}, policy_{policy}, calls_{config.strategies},
        overflowing_{config.strategies} {}

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  ~Engine() = default;

  [[nodiscard]] KernelServices& kernel() noexcept { return k_; }
  [[nodiscard]] const KernelServices& kernel() const noexcept { return k_; }

  // Processes one input event.
  [[nodiscard]] core::Status step(const core::EventKey& key, const model::Event& event) {
    k_.current = key;
    k_.logs.clear();
    k_.trading.begin_step();
    const Watched before = watched();
    const std::size_t first_output = k_.outputs.size();
    const core::Status s = std::visit(
        [this](const auto& e) {
          if constexpr (detail::kMarketData<std::decay_t<decltype(e)>>) {
            note_market_data();
          }
          return this->dispatch(e);
        },
        event);
    deliver_order_events();
    cover_submits(first_output);
    arm_algo_timer();
    k_.timers.prune(); // between steps the next timer is the heap's top (TimerQueue::peek)
    log_changes(before, first_output);
    return s;
  }

  // The log records of the last step (strategy/telemetry.hpp).
  [[nodiscard]] std::span<const strategy::LogRecord> logs() const noexcept {
    return k_.logs.span();
  }

  [[nodiscard]] std::span<const model::Output> outputs() const noexcept {
    return k_.outputs.span();
  }
  [[nodiscard]] std::span<const strategy::StrategyFailure> failures() const noexcept {
    return k_.failures.span();
  }
  void clear_failures() noexcept { k_.failures.clear(); }

  // Writes this step's outputs to `sink` keyed by the causing input, then clears them.
  template <CommandSink C> [[nodiscard]] core::Status flush_outputs(C& sink) {
    for (std::size_t i = 0; i < k_.outputs.size(); ++i) {
      const core::EventKey key{k_.current.ts, static_cast<std::uint16_t>(i), k_.current.seq};
      const core::Status s = sink.emit(key, k_.outputs[i]);
      if (!core::ok(s)) {
        return s;
      }
    }
    k_.outputs.clear();
    return core::Status::Ok;
  }
  void clear_outputs() noexcept { k_.outputs.clear(); }

  // The next timer due, without firing it; the node turns it into a TimerFired input.
  [[nodiscard]] bool next_timer(core::FiredTimer& out) const noexcept {
    return k_.timers.peek(out);
  }

  [[nodiscard]] bool halt_requested() const noexcept { return k_.halt_requested; }
  // An admin shutdown: the driver stops the node the configured way.
  [[nodiscard]] bool stop_requested() const noexcept { return k_.stop_requested; }

  // Orders not yet in a terminal state; the drain at shutdown waits until there are none.
  [[nodiscard]] std::uint32_t open_orders() const noexcept {
    std::uint32_t n = 0;
    for (std::uint32_t slot = 0; slot < k_.instruments.size(); ++slot) {
      n += k_.trading.oms.open_quantity(slot).orders;
    }
    return n;
  }

  // ---- EngineState snapshots (docs/architecture.md section 16.3) ----------------------------

  [[nodiscard]] std::size_t strategy_count() const noexcept { return ss_->size(); }

  // Every strategy describes its own state (strategy.hpp): a restored snapshot then continues
  // exactly as this engine would.
  [[nodiscard]] bool snapshot_complete() {
    for (std::size_t i = 0; i < ss_->size(); ++i) {
      if (!strategy_has_state(static_cast<StrategyIndex>(i))) {
        return false;
      }
    }
    return true;
  }

  // Appends the kernel's state and each strategy's own to `out`. Between steps only.
  [[nodiscard]] core::Status save_state(std::vector<std::byte>& out) {
    core::StateWriter w{out};
    state(w);
    const auto count = static_cast<std::uint16_t>(ss_->size());
    w.u16(count);
    std::vector<std::byte> inner;
    for (std::uint16_t i = 0; i < count; ++i) {
      const bool has = strategy_has_state(i);
      w.u8(has ? 1U : 0U);
      if (!has) {
        continue;
      }
      inner.clear();
      core::StateWriter iw{inner};
      if constexpr (requires { ss_->save_state(i, iw); }) {
        const core::Status s = ss_->save_state(i, iw);
        if (!core::ok(s)) {
          return s;
        }
      }
      w.u32(static_cast<std::uint32_t>(inner.size()));
      w.raw(inner);
    }
    return w.status();
  }

  // Restores what save_state wrote into this engine, built from the same configuration and the
  // same strategies. On failure the engine is unusable and must be discarded.
  [[nodiscard]] core::Status load_state(std::span<const std::byte> in) {
    core::StateReader r{in};
    state(r);
    const std::uint16_t count = r.u16();
    if (!r.ok_state()) {
      return r.status();
    }
    if (count != ss_->size()) {
      return core::Status::InvalidArgument;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
      const bool has = r.u8() == 1U;
      if (!r.ok_state()) {
        return r.status();
      }
      if (has != strategy_has_state(i)) {
        return core::Status::InvalidState;
      }
      if (!has) {
        continue;
      }
      const std::uint32_t n = r.u32();
      const std::span<const std::byte> bytes = r.raw(n);
      if (!r.ok_state()) {
        return r.status();
      }
      core::StateReader ir{bytes};
      if constexpr (requires { ss_->load_state(i, ir); }) {
        const core::Status s = ss_->load_state(i, ir);
        if (!core::ok(s)) {
          return s;
        }
      }
      if (ir.remaining() != 0) {
        return core::Status::InvalidArgument;
      }
    }
    return r.remaining() == 0 ? core::Status::Ok : core::Status::InvalidArgument;
  }

private:
  // ---- dispatch -----------------------------------------------------------------------------

  template <typename T> core::Status dispatch(const T& e) {
    if constexpr (std::is_same_v<T, model::TradeTick>) {
      return on_trade(e);
    } else if constexpr (std::is_same_v<T, model::QuoteTick>) {
      return on_quote(e);
    } else if constexpr (std::is_same_v<T, model::OrderBookDeltas>) {
      return on_deltas(e);
    } else if constexpr (std::is_same_v<T, model::Bar>) {
      return on_bar(e);
    } else if constexpr (std::is_same_v<T, model::MarkPriceUpdate>) {
      return on_mark(e);
    } else if constexpr (std::is_same_v<T, model::IndexPriceUpdate>) {
      return simple(e, data::DataKind::IndexPrice);
    } else if constexpr (std::is_same_v<T, model::FundingRateUpdate>) {
      return on_funding(e);
    } else if constexpr (std::is_same_v<T, model::InstrumentStatus>) {
      return on_status(e);
    } else if constexpr (std::is_same_v<T, model::InstrumentClose>) {
      return simple(e, data::DataKind::Close);
    } else if constexpr (std::is_same_v<T, model::LiquidationOrder>) {
      return simple(e, data::DataKind::Liquidation);
    } else {
      return dispatch_other(e);
    }
  }

  // Everything that is not market data: kernel inputs, venue events, definitions, control.
  template <typename T> core::Status dispatch_other(const T& e) {
    if constexpr (std::is_same_v<T, model::NodeLifecycle>) {
      return on_lifecycle(e);
    } else if constexpr (std::is_same_v<T, model::TimerFired>) {
      return on_timer_fired(e);
    } else if constexpr (std::is_same_v<T, model::BatchEnd>) {
      flush_batch();
      return core::Status::Ok;
    } else if constexpr (std::is_same_v<T, model::StrategyError>) {
      return on_strategy_error(e);
    } else if constexpr (std::is_same_v<T, model::RateLimitFeedback>) {
      if (e.kind == model::RateLimitKind::Orders) {
        k_.trading.risk.limiter().feedback(e.ts_init, e.interval_ns, e.used);
      }
      return core::Status::Ok; // request weight is the adapter's (per IP, not per account)
    } else if constexpr (detail::is_alternative_v<T, model::Instrument>) {
      return k_.define_instrument(model::Instrument{e});
    } else if constexpr (detail::is_alternative_v<T, model::OrderEvent>) {
      return on_venue_input(model::OrderEvent{e});
    } else if constexpr (std::is_same_v<T, model::AccountState>) {
      return on_venue_input(e);
    } else if constexpr (std::is_same_v<T, model::ConnectionStatus>) {
      return on_connection(e);
    } else if constexpr (std::is_same_v<T, model::VenueSnapshot>) {
      ReconcileHost host{*this};
      return k_.trading.reconciler.reconcile(host, e);
    } else if constexpr (std::is_same_v<T, model::Shutdown>) {
      return on_shutdown(e);
    } else if constexpr (std::is_same_v<T, model::AdminCommand>) {
      return on_admin(e);
    } else if constexpr (std::is_same_v<T, model::RunStart>) {
      return on_run_start(e);
    } else if constexpr (std::is_same_v<T, model::ParamUpdate>) {
      return on_param_update(e);
    } else {
      return core::Status::Ok;
    }
  }

  core::Status on_bar(const model::Bar& e) {
    if (const auto key = k_.find_bar_type(e.bar_type)) {
      deliver(*key, data::DataKind::Bar, e, e.ts_init);
    }
    return core::Status::Ok;
  }

  [[nodiscard]] bool slot_of(const model::InstrumentId& id, std::uint32_t& slot) const noexcept {
    model::InstrumentSlot s;
    if (!core::ok(k_.instruments.find(id, s))) {
      return false; // nobody declared interest in this instrument
    }
    slot = s.value;
    return true;
  }

  template <typename T> core::Status simple(const T& e, data::DataKind kind) {
    std::uint32_t slot = 0;
    if (slot_of(e.instrument_id, slot)) {
      deliver(slot, kind, e, e.ts_init);
    }
    return core::Status::Ok;
  }

  core::Status on_status(const model::InstrumentStatus& e) {
    std::uint32_t slot = 0;
    if (slot_of(e.instrument_id, slot)) {
      k_.trading.risk.on_status(slot, e);
      deliver(slot, data::DataKind::Status, e, e.ts_init);
    }
    return core::Status::Ok;
  }

  // The monitors see the new valuation before subscribers do.
  core::Status on_mark(const model::MarkPriceUpdate& e) {
    std::uint32_t slot = 0;
    if (!slot_of(e.instrument_id, slot)) {
      return core::Status::Ok;
    }
    k_.trading.portfolio.set_mark(slot, e.value);
    const core::Status s = k_.trading.monitor(k_.current, k_.outputs);
    if (!core::ok(s)) {
      return s;
    }
    deliver(slot, data::DataKind::MarkPrice, e, e.ts_init);
    return core::Status::Ok;
  }

  // Funding settles before subscribers see the update, so a strategy reading its position in
  // on_funding_rate sees the payment.
  core::Status on_funding(const model::FundingRateUpdate& e) {
    std::uint32_t slot = 0;
    if (!slot_of(e.instrument_id, slot)) {
      return core::Status::Ok;
    }
    core::Status s = k_.trading.on_funding(k_.current, slot, e);
    if (core::ok(s)) {
      s = k_.trading.monitor(k_.current, k_.outputs);
    }
    if (!core::ok(s)) {
      return s;
    }
    deliver(slot, data::DataKind::FundingRate, e, e.ts_init);
    return core::Status::Ok;
  }

  core::Status on_trade(const model::TradeTick& t) {
    std::uint32_t slot = 0;
    if (!slot_of(t.instrument_id, slot)) {
      return core::Status::Ok;
    }
    k_.trading.portfolio.note_trade(slot, t.price);
    deliver(slot, data::DataKind::Trade, t, t.ts_init);
    for (std::size_t f = 0; f < k_.features.size(); ++f) {
      data::Feature& feature = k_.features.at(static_cast<model::FeatureId>(f));
      if (feature.slot().value != slot || !data::uses_trades(feature.spec().kind)) {
        continue;
      }
      model::Decimal value;
      bool produced = false;
      const core::Status s = feature.on_trade(t, value, produced);
      if (!core::ok(s)) {
        return s;
      }
      if (produced) {
        deliver_feature(static_cast<model::FeatureId>(f), value, t.ts_event, t.ts_init);
      }
    }
    return feed_aggregators(slot, t);
  }

  core::Status on_quote(const model::QuoteTick& q) {
    std::uint32_t slot = 0;
    if (!slot_of(q.instrument_id, slot)) {
      return core::Status::Ok;
    }
    const bool top = q.bid_price.raw() > 0 && q.ask_price.raw() > q.bid_price.raw();
    if (top) {
      k_.trading.set_top(slot, strategy::AlgoTop{q.bid_price, q.ask_price});
    }
    data::OrderBook* book = k_.book_for_update(slot, q.bid_price, q.bid_size.precision());
    const bool l1 = book != nullptr && book->type() == model::BookType::L1_MBP;
    if (l1) {
      const core::Status s = book->apply(q);
      if (!core::ok(s)) {
        return s;
      }
    }
    deliver(slot, data::DataKind::Quote, q, q.ts_init);
    if (l1) {
      deliver(slot, data::DataKind::Book, data::BookView{q.instrument_id, book}, q.ts_init);
    }
    for (std::size_t f = 0; f < k_.features.size(); ++f) {
      data::Feature& feature = k_.features.at(static_cast<model::FeatureId>(f));
      if (feature.slot().value != slot || data::uses_trades(feature.spec().kind)) {
        continue;
      }
      model::Decimal value;
      bool produced = false;
      const core::Status s = feature.on_quote(q, value, produced);
      if (!core::ok(s)) {
        return s;
      }
      if (produced) {
        deliver_feature(static_cast<model::FeatureId>(f), value, q.ts_event, q.ts_init);
      }
    }
    if (top) {
      const core::Status s = k_.trading.on_top(k_.current, slot, k_.outputs);
      if (!core::ok(s)) {
        return s;
      }
    }
    return feed_aggregators(slot, q);
  }

  core::Status on_deltas(const model::OrderBookDeltas& d) {
    std::uint32_t slot = 0;
    if (!slot_of(d.instrument_id, slot) || d.deltas.empty()) {
      return core::Status::Ok;
    }
    const model::OrderBookDelta& first = d.deltas.front();
    data::OrderBook* book =
        k_.book_for_update(slot, first.order.price, first.order.size.precision());
    bool top = false;
    if (book != nullptr && book->type() == model::BookType::L2_MBP) {
      const core::Status s = book->apply(d);
      if (!core::ok(s)) {
        return s;
      }
      data::BookLevel bid;
      data::BookLevel ask;
      top = book->best_bid(bid) && book->best_ask(ask) && ask.price.raw() > bid.price.raw();
      if (top) {
        k_.trading.set_top(slot, strategy::AlgoTop{bid.price, ask.price});
      }
    }
    deliver(slot, data::DataKind::BookDeltas, d, d.ts_init);
    if (book != nullptr) {
      deliver(slot, data::DataKind::Book, data::BookView{d.instrument_id, book}, d.ts_init);
    }
    return top ? k_.trading.on_top(k_.current, slot, k_.outputs) : core::Status::Ok;
  }

  template <typename T> core::Status feed_aggregators(std::uint32_t slot, const T& update) {
    for (std::size_t i = 0; i < k_.aggregators.size(); ++i) {
      strategy::AggregatorState& a = k_.aggregators[i];
      if (a.slot != slot || !takes(a, update)) {
        continue;
      }
      const core::Status s = aggregate(static_cast<std::uint32_t>(i), update);
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  // Last-price bars aggregate trades; bid, ask and mid bars aggregate quotes.
  template <typename T>
  [[nodiscard]] static bool takes(const strategy::AggregatorState& a, const T& /*update*/) {
    const bool trades = a.bar_type.spec.price_type == model::PriceType::Last;
    return std::is_same_v<T, model::TradeTick> == trades;
  }

  // Creates the aggregator on its first update (precisions come from the data), feeds it, and
  // delivers the bars it completes.
  template <typename T> core::Status aggregate(std::uint32_t index, const T& update) {
    strategy::AggregatorState& a = k_.aggregators[index];
    core::Status s = core::Status::Ok;
    if (!a.ready) {
      if constexpr (std::is_same_v<T, model::TradeTick>) {
        s = data::BarAggregator::create(a.bar_type, update.price.precision(),
                                        update.size.precision(), a.aggregator);
      } else {
        s = data::BarAggregator::create(a.bar_type, update.bid_price.precision(),
                                        update.bid_size.precision(), a.aggregator);
      }
      if (!core::ok(s)) {
        return s;
      }
      a.ready = true;
    }
    std::array<model::Bar, 8> bars{};
    std::size_t n = 0;
    if constexpr (std::is_same_v<T, model::TradeTick>) {
      s = a.aggregator.on_trade(update, bars, n);
    } else {
      s = a.aggregator.on_quote(update, bars, n);
    }
    if (!core::ok(s)) {
      return s;
    }
    for (std::size_t b = 0; b < n; ++b) {
      deliver(a.bar_key, data::DataKind::Bar, bars[b], bars[b].ts_init);
    }
    return arm_close_timer(index);
  }

  core::Status arm_close_timer(std::uint32_t index) {
    strategy::AggregatorState& a = k_.aggregators[index];
    const std::optional<core::UnixNanos> close = a.aggregator.next_close();
    if (!close || close->value() == a.armed_deadline) {
      return core::Status::Ok;
    }
    if (a.armed_deadline != 0) {
      static_cast<void>(k_.timers.cancel(a.timer));
    }
    a.armed_deadline = close->value();
    return k_.timers.schedule(*close, core::DurationNanos{},
                              core::TimerKey{strategy::kKernelTimerOwner, index}, a.timer);
  }

  // ---- delivery -----------------------------------------------------------------------------

  static strategy::PendingValue pending_of(const data::BookView& v, std::uint32_t slot) {
    static_cast<void>(v);
    return strategy::BookMark{slot};
  }
  template <typename T>
  static strategy::PendingValue pending_of(const T& v, std::uint32_t /*slot*/) {
    if constexpr (std::is_same_v<T, model::OrderBookDeltas>) {
      return std::monostate{}; // not conflatable (rejected at subscription)
    } else {
      return v;
    }
  }

  template <typename T>
  void deliver(std::uint32_t row, data::DataKind kind, const T& value, core::UnixNanos ts) {
    calls_.clear();
    overflowing_.clear();
    for (data::Subscriber& sub : k_.matrix.subscribers(row, kind)) {
      if (k_.is_disabled(sub.strategy)) {
        continue;
      }
      switch (sub.cadence.mode) {
      case data::Cadence::Mode::Every:
        static_cast<void>(calls_.push_back(sub.strategy));
        break;
      case data::Cadence::Mode::Sampled: {
        const std::uint64_t period = ts.value() / sub.cadence.period.value();
        if (period != sub.last_period) {
          sub.last_period = period;
          static_cast<void>(calls_.push_back(sub.strategy));
        }
        break;
      }
      case data::Cadence::Mode::Conflated:
        store_pending(sub.buffer, pending_of(value, row));
        break;
      case data::Cadence::Mode::OnBatch:
        if constexpr (std::is_same_v<T, model::TradeTick> || std::is_same_v<T, model::QuoteTick>) {
          if (!append_batch(sub.buffer, value)) {
            static_cast<void>(overflowing_.push_back(sub.buffer));
          }
        }
        break;
      }
    }
    if constexpr (std::is_same_v<T, model::FeatureUpdate>) {
      if (!calls_.empty()) {
        static_cast<void>(k_.outputs.emplace_back(std::in_place_type<model::FeatureUpdate>, value));
      }
    }
    const strategy::DataView view{&value};
    for (std::size_t i = 0; i < calls_.size(); ++i) {
      call_data(calls_[i], view);
    }
    if constexpr (std::is_same_v<T, model::TradeTick> || std::is_same_v<T, model::QuoteTick>) {
      for (std::size_t i = 0; i < overflowing_.size(); ++i) {
        flush_batch_buffer(overflowing_[i]); // a full buffer is delivered early, deterministically
        static_cast<void>(append_batch(overflowing_[i], value));
      }
    }
  }

  void deliver_feature(model::FeatureId id, model::Decimal value, core::UnixNanos ts_event,
                       core::UnixNanos ts_init) {
    deliver(id, data::DataKind::Feature, model::FeatureUpdate{id, value, ts_event, ts_init},
            ts_init);
  }

  void store_pending(std::uint32_t buffer, strategy::PendingValue value) {
    strategy::Pending& p = k_.pending[buffer];
    p.value = value;
    if (!p.dirty) {
      p.dirty = true;
      static_cast<void>(k_.dirty_pending.push_back(buffer));
    }
  }

  template <typename T> bool append_batch(std::uint32_t buffer, const T& value) {
    strategy::BatchBuffer& b = k_.batches[buffer];
    core::Status s = core::Status::Ok;
    if constexpr (std::is_same_v<T, model::TradeTick>) {
      s = b.trades.push_back(value);
    } else {
      s = b.quotes.push_back(value);
    }
    if (!core::ok(s)) {
      return false;
    }
    if (!b.dirty) {
      b.dirty = true;
      static_cast<void>(k_.dirty_batches.push_back(buffer));
    }
    return true;
  }

  void flush_batch_buffer(std::uint32_t buffer) {
    strategy::BatchBuffer& b = k_.batches[buffer];
    if (!k_.is_disabled(b.strategy) && (!b.trades.empty() || !b.quotes.empty())) {
      strategy::Context ctx{k_, b.strategy};
      core::Status s = core::Status::Ok;
      if (b.kind == data::DataKind::Trade) {
        const strategy::TradeBatch batch{b.instrument_id, b.trades.span()};
        s = ss_->on_batch(b.strategy, ctx, strategy::BatchView{&batch});
      } else {
        const strategy::QuoteBatch batch{b.instrument_id, b.quotes.span()};
        s = ss_->on_batch(b.strategy, ctx, strategy::BatchView{&batch});
      }
      if (!core::ok(s)) {
        k_.fail(b.strategy, s);
      }
    }
    b.trades.clear();
    b.quotes.clear();
  }

  // BatchEnd: conflated updates, then OnBatch buffers, each in the order they became pending.
  void flush_batch() {
    for (std::size_t i = 0; i < k_.dirty_pending.size(); ++i) {
      strategy::Pending& p = k_.pending[k_.dirty_pending[i]];
      p.dirty = false;
      if (k_.is_disabled(p.strategy)) {
        continue;
      }
      const strategy::PendingValue value = p.value;
      std::visit(
          [&](const auto& v) {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, strategy::BookMark>) {
              const data::OrderBook* book = k_.books[v.slot] ? &*k_.books[v.slot] : nullptr;
              if (book != nullptr) {
                const data::BookView view{k_.instruments.id(model::InstrumentSlot{v.slot}), book};
                call_data(p.strategy, strategy::DataView{&view});
              }
            } else if constexpr (!std::is_same_v<V, std::monostate>) {
              if constexpr (std::is_same_v<V, model::FeatureUpdate>) {
                static_cast<void>(
                    k_.outputs.emplace_back(std::in_place_type<model::FeatureUpdate>, v));
              }
              call_data(p.strategy, strategy::DataView{&v});
            }
          },
          value);
    }
    k_.dirty_pending.clear();
    for (std::size_t i = 0; i < k_.dirty_batches.size(); ++i) {
      const std::uint32_t buffer = k_.dirty_batches[i];
      k_.batches[buffer].dirty = false;
      flush_batch_buffer(buffer);
    }
    k_.dirty_batches.clear();
  }

  void call_data(StrategyIndex s, const strategy::DataView& view) {
    strategy::Context ctx{k_, s};
    const core::Status status = ss_->on_data(s, ctx, view);
    if (!core::ok(status)) {
      k_.fail(s, status);
    }
  }

  // ---- orders -------------------------------------------------------------------------------

  // `applied`, when given, tells whether the OMS applied the event.
  core::Status on_venue_order_event(const model::OrderEvent& e, bool* applied = nullptr) {
    StrategyIndex owner = 0;
    core::Status algo = core::Status::Ok;
    const bool ok = k_.trading.on_venue_event(k_.current, e, owner, k_.outputs, algo);
    if (applied != nullptr) {
      *applied = ok;
    }
    if (!ok) {
      return core::Status::Ok; // refused and unknown events are counted, not fatal
    }
    if (!core::ok(algo)) {
      return algo;
    }
    if (std::holds_alternative<model::OrderFilled>(e) ||
        std::holds_alternative<model::OrderFillVoided>(e)) {
      const core::Status s = k_.trading.monitor(k_.current, k_.outputs);
      if (!core::ok(s)) {
        return s;
      }
    }
    deliver_order_event(owner, e);
    return core::Status::Ok;
  }

  void deliver_order_event(StrategyIndex s, const model::OrderEvent& e) {
    if (s >= ss_->size() || k_.is_disabled(s)) {
      return;
    }
    strategy::Context ctx{k_, s};
    const core::Status status = ss_->on_order_event(s, ctx, e);
    if (!core::ok(status)) {
      k_.fail(s, status);
    }
  }

  void deliver_position_event(StrategyIndex s, const model::PositionEvent& e) {
    if (s >= ss_->size() || k_.is_disabled(s)) {
      return;
    }
    strategy::Context ctx{k_, s};
    const core::Status status = ss_->on_position_event(s, ctx, e);
    if (!core::ok(status)) {
      k_.fail(s, status);
    }
  }

  // The queue has fixed capacity and never reallocates, so entries stay put while callbacks
  // append to it.
  void deliver_order_events() {
    core::FixedVector<strategy::PendingEvent>& events = k_.trading.events;
    for (std::size_t i = 0; i < events.size(); ++i) {
      const strategy::PendingEvent& p = events[i];
      if (const auto* order = std::get_if<model::OrderEvent>(&p.event)) {
        deliver_order_event(p.strategy, *order);
      } else if (const auto* position = std::get_if<model::PositionEvent>(&p.event)) {
        deliver_position_event(p.strategy, *position);
      }
    }
    events.clear();
  }

  // ---- reconciliation -----------------------------------------------------------------------

  // A venue order event or account state: held while the account is not synced.
  core::Status on_venue_input(const model::OrderEvent& e) {
    if (k_.trading.reconciler.holding()) {
      return k_.trading.reconciler.hold(e);
    }
    return on_venue_order_event(e);
  }
  core::Status on_venue_input(const model::AccountState& e) {
    if (k_.trading.reconciler.holding()) {
      return k_.trading.reconciler.hold(e);
    }
    return k_.trading.portfolio.set_account(e);
  }

  // The user data stream going down halts trading at once; Running releases the hold once the
  // account is synced again.
  core::Status on_connection(const model::ConnectionStatus& e) {
    k_.health.apply(e.kind, e.up);
    if (e.kind == model::ConnectionKind::MarketData && e.up) {
      k_.freshness.last_market_data = k_.current.ts; // silence counts from the connection
      arm_freshness();
    }
    if (e.kind == model::ConnectionKind::UserStream && k_.trading.reconciler.on_connection(e.up)) {
      static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::SyncStarted));
    }
    return core::Status::Ok;
  }

  // Every strategy learns of the reconciliation, started or not: at start it comes before
  // on_start.
  void deliver_reconciled(const model::ReconcileOutcome& outcome) {
    for (std::size_t i = 0; i < ss_->size(); ++i) {
      const auto s = static_cast<StrategyIndex>(i);
      if (k_.is_disabled(s)) {
        continue;
      }
      strategy::Context ctx{k_, s};
      const core::Status status = ss_->on_reconciled(s, ctx, outcome);
      if (!core::ok(status)) {
        k_.fail(s, status);
      }
    }
  }

  // What reconciliation reads and changes (execution::ReconcileHost).
  class ReconcileHost {
  public:
    explicit ReconcileHost(Engine& engine) noexcept : e_{&engine} {}

    [[nodiscard]] const execution::Oms& oms() const noexcept { return e_->k_.trading.oms; }
    [[nodiscard]] portfolio::Portfolio& portfolio() noexcept { return e_->k_.trading.portfolio; }
    [[nodiscard]] const model::Instrument* definition(std::uint32_t slot) const noexcept {
      return e_->k_.trading.definition(slot);
    }
    [[nodiscard]] std::uint32_t slot_of(const model::InstrumentId& id) const noexcept {
      std::uint32_t slot = 0;
      return e_->slot_of(id, slot) ? slot : execution::kNoIndex;
    }
    [[nodiscard]] model::OrderEventHeader header(std::uint32_t index,
                                                 core::UnixNanos ts_event) noexcept {
      return e_->k_.trading.venue_header(e_->k_.current, index, ts_event);
    }
    [[nodiscard]] const model::AccountId& account_id() const noexcept {
      return e_->k_.trading.account_id;
    }
    [[nodiscard]] const model::ClientOrderIdGenerator& ids() const noexcept {
      return e_->k_.trading.ids;
    }
    [[nodiscard]] core::UnixNanos now() const noexcept { return e_->k_.current.ts; }
    [[nodiscard]] std::size_t outputs_left() const noexcept {
      return e_->k_.outputs.capacity() - e_->k_.outputs.size();
    }
    [[nodiscard]] core::Status apply(const model::OrderEvent& e, bool& applied) {
      const core::Status s = e_->on_venue_order_event(e, &applied);
      e_->deliver_order_events();
      return s;
    }
    [[nodiscard]] bool emit(const model::Output& o) noexcept {
      return core::ok(e_->k_.outputs.push_back(o));
    }
    void note_venue_time(std::uint32_t index, core::UnixNanos ts) noexcept {
      execution::OrderRecord& r = e_->k_.trading.oms.at(index);
      r.ts_venue = ts > r.ts_venue ? ts : r.ts_venue;
    }
    void reconciled(const model::ReconcileOutcome& outcome) {
      e_->deliver_reconciled(outcome);
      e_->deliver_order_events();
    }
    void check_failed() noexcept {
      static_cast<void>(e_->k_.trading.risk.apply(risk::TradingTrigger::SoftLimit));
    }

  private:
    Engine* e_;
  };
  static_assert(execution::ReconcileHost<ReconcileHost>);

  // ---- lifecycle, timers, errors ------------------------------------------------------------

  core::Status on_lifecycle(const model::NodeLifecycle& e) {
    k_.node_state = e.to;
    k_.trading.on_lifecycle(e.from, e.to);
    countdown_on_lifecycle(e.to);
    if (e.to == model::NodeState::Running && !started_) {
      started_ = true;
      for_each_active(
          [this](StrategyIndex s, strategy::Context& ctx) { return ss_->on_start(s, ctx); });
    } else if (e.to == model::NodeState::Stopping && !k_.stopped) {
      if (started_) {
        flush_batch();
        for_each_active(
            [this](StrategyIndex s, strategy::Context& ctx) { return ss_->on_stop(s, ctx); });
      }
      k_.stopped = true;
    }
    return core::Status::Ok;
  }

  template <typename Ar> void state(Ar& ar) { ar(k_, started_); }

  [[nodiscard]] bool strategy_has_state(StrategyIndex i) {
    if constexpr (requires { ss_->has_state(i); }) {
      return ss_->has_state(i);
    } else {
      return false;
    }
  }

  // Calls `call(s, ctx)` for every strategy still receiving callbacks; a failure is noted.
  template <typename F> void for_each_active(F&& call) {
    for (std::size_t i = 0; i < ss_->size(); ++i) {
      const auto s = static_cast<StrategyIndex>(i);
      if (k_.is_disabled(s)) {
        continue;
      }
      strategy::Context ctx{k_, s};
      const core::Status status = call(s, ctx);
      if (!core::ok(status)) {
        k_.fail(s, status);
      }
    }
  }

  // A live run's first input (sections 4.1 and 16.3): new ClientOrderIds take its epoch. When
  // the run continues an earlier run's state, what belonged to that process goes: its
  // connections are gone, so the account reconciles again (trading halts until it has);
  // shutdown, stop and halt requests and the Degraded mark clear; the lifecycle restarts from
  // Init (on_start is not called again: the strategies have started); the countdown timer is
  // disarmed (the venue's own countdown keeps running until the new session renews or lets it
  // fire); updates buffered for a batch the earlier run never closed are dropped. In a fresh
  // kernel all of that is already so.
  core::Status on_run_start(const model::RunStart& e) {
    model::ClientOrderIdGenerator ids;
    const core::Status s =
        model::ClientOrderIdGenerator::create(k_.trading.ids.node_tag(), e.epoch, ids);
    if (!core::ok(s)) {
      return s;
    }
    k_.trading.ids = ids;
    k_.node_state = model::NodeState::Init;
    k_.stopped = false;
    k_.stop_requested = false;
    k_.halt_requested = false;
    k_.shutdown.reset();
    k_.health = execution::ConnectionHealth{};
    static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::Recovered));
    if (k_.trading.reconciler.phase() != execution::SyncPhase::Local &&
        k_.trading.reconciler.on_connection(false)) {
      static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::SyncStarted));
    }
    if (k_.countdown.armed) {
      static_cast<void>(k_.timers.cancel(k_.countdown.timer));
      k_.countdown.armed = false;
    }
    if (k_.freshness.armed) {
      static_cast<void>(k_.timers.cancel(k_.freshness.timer));
      k_.freshness.armed = false;
    }
    k_.countdown.running = false;
    for (std::size_t slot = 0; slot < k_.countdown.live.size(); ++slot) {
      k_.countdown.live[slot] = 0;
    }
    for (std::size_t i = 0; i < k_.dirty_pending.size(); ++i) {
      k_.pending[k_.dirty_pending[i]].dirty = false;
    }
    k_.dirty_pending.clear();
    for (std::size_t i = 0; i < k_.dirty_batches.size(); ++i) {
      strategy::BatchBuffer& b = k_.batches[k_.dirty_batches[i]];
      b.dirty = false;
      b.trades.clear();
      b.quotes.clear();
    }
    k_.dirty_batches.clear();
    return core::Status::Ok;
  }

  // An operator's command (section 19.3). The TradingState changes are the TradingState spec's
  // admin triggers; cancel_all is the KillSwitch without a state change.
  core::Status on_admin(const model::AdminCommand& e) {
    switch (e.action) {
    case model::AdminAction::Halt:
      static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::AdminHalt));
      return core::Status::Ok;
    case model::AdminAction::Reduce:
      static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::AdminReduce));
      return core::Status::Ok;
    case model::AdminAction::Resume:
      static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::AdminResume));
      return core::Status::Ok;
    case model::AdminAction::CancelAll:
      return k_.trading.kill_switch(k_.current, k_.outputs);
    case model::AdminAction::Shutdown:
      k_.stop_requested = true;
      return core::Status::Ok;
    case model::AdminAction::Snapshot:
      return core::Status::Ok; // the node, and a replay, take it at the next batch end
    }
    return core::Status::InvalidArgument;
  }

  // The node is shutting down (section 19.4). CancelAllThenExit halts trading and cancels every
  // open order; the driver then waits in Stopping for the venue to confirm. ExitKeepOrders leaves
  // the orders, so the countdown is disarmed at Stopped whatever is open.
  core::Status on_shutdown(const model::Shutdown& e) {
    if (k_.shutdown) {
      return core::Status::Ok; // the first one decides
    }
    k_.shutdown = e.mode;
    if (e.mode != model::ShutdownMode::CancelAllThenExit) {
      return core::Status::Ok;
    }
    static_cast<void>(k_.trading.risk.apply(risk::TradingTrigger::AdminHalt));
    return k_.trading.kill_switch(k_.current, k_.outputs);
  }

  core::Status on_timer_fired(const model::TimerFired& e) {
    core::FiredTimer fired;
    if (!k_.timers.pop_due(e.deadline, fired) || !(fired.key == e.key) ||
        !(fired.deadline == e.deadline)) {
      return core::Status::InvalidState; // the recorded timer is not the one due: divergence
    }
    if (e.key.owner == strategy::kKernelTimerOwner && e.key.id == strategy::kAlgoTimerId) {
      k_.algo_timer.armed = false;
      k_.trading.algo_wake_changed = true;
      return k_.trading.on_algo_timer(k_.current, e.deadline, k_.outputs);
    }
    if (e.key.owner == strategy::kKernelTimerOwner && e.key.id == strategy::kStaleTimerId) {
      check_freshness();
      return core::Status::Ok;
    }
    if (e.key.owner == strategy::kKernelTimerOwner && e.key.id == strategy::kCountdownTimerId) {
      k_.countdown.armed = false;
      if (k_.countdown.running) {
        renew_countdown();
      }
      return core::Status::Ok;
    }
    if (e.key.owner == strategy::kKernelTimerOwner) {
      if (e.key.id >= k_.aggregators.size()) {
        return core::Status::InvalidState;
      }
      strategy::AggregatorState& a = k_.aggregators[e.key.id];
      a.armed_deadline = 0;
      model::Bar bar;
      if (a.aggregator.on_time(e.deadline, bar)) {
        deliver(a.bar_key, data::DataKind::Bar, bar, bar.ts_init);
      }
      return core::Status::Ok;
    }
    for (std::size_t i = 0; i < k_.timer_entries.size(); ++i) {
      if (k_.timer_entries[i].key == e.key) {
        if (!k_.timer_entries[i].periodic) {
          k_.remove_timer_entry(i);
        }
        break;
      }
    }
    const auto s = static_cast<StrategyIndex>(e.key.owner);
    if (s < ss_->size() && !k_.is_disabled(s)) {
      strategy::Context ctx{k_, s};
      const core::Status status = ss_->on_timer(s, ctx, e.key, e.deadline);
      if (!core::ok(status)) {
        k_.fail(s, status);
      }
    }
    return core::Status::Ok;
  }

  // ---- the execution algorithms' timer ------------------------------------------------------
  // One kernel timer at the earliest time an algorithm asked for (ctx.wake_at), re-armed after
  // every step that changed one.

  void arm_algo_timer() {
    if (!k_.trading.algo_wake_changed) {
      return;
    }
    k_.trading.algo_wake_changed = false;
    const std::optional<core::UnixNanos> next = k_.trading.next_algo_wake();
    strategy::AlgoTimerState& t = k_.algo_timer;
    if (t.armed && (!next || next->value() != t.deadline)) {
      static_cast<void>(k_.timers.cancel(t.timer));
      t.armed = false;
    }
    if (next && !t.armed) {
      t.armed = core::ok(k_.timers.schedule(
          *next, core::DurationNanos{},
          core::TimerKey{strategy::kKernelTimerOwner, strategy::kAlgoTimerId}, t.timer));
      t.deadline = next->value();
    }
  }

  // ---- the venue-side dead man's switch (section 10.3) ---------------------------------------
  // While Running, a kernel timer renews the countdown of every instrument with open orders, a
  // quarter of the countdown apart; out of Running the renewals stop, so a node that stays out of
  // sync for the whole countdown has its orders canceled by the venue. An order submitted for an
  // instrument not renewed since the last renewal gets its countdown in the same step.

  [[nodiscard]] std::uint32_t countdown_ms() const noexcept {
    return k_.trading.risk.config().countdown_cancel_ms;
  }

  void emit_countdown(std::uint32_t slot) {
    model::CountdownCancelAll c;
    c.instrument_id = k_.instruments.id(model::InstrumentSlot{slot});
    c.countdown_ms = countdown_ms();
    c.ts_init = k_.current.ts;
    if (core::ok(k_.outputs.emplace_back(std::in_place_type<model::CountdownCancelAll>, c))) {
      k_.countdown.live[slot] = 1;
    }
  }

  void countdown_on_lifecycle(model::NodeState to) {
    if (countdown_ms() == 0) {
      return;
    }
    k_.countdown.running = to == model::NodeState::Running;
    if (k_.countdown.running && !k_.countdown.armed) {
      renew_countdown();
    } else if (to == model::NodeState::Stopped && k_.shutdown &&
               (*k_.shutdown == model::ShutdownMode::ExitKeepOrders || open_orders() == 0)) {
      disarm_countdown();
    }
  }

  void renew_countdown() {
    for (std::uint32_t slot = 0; slot < k_.instruments.size() && slot < k_.countdown.live.size();
         ++slot) {
      k_.countdown.live[slot] = 0;
      if (k_.trading.oms.open_quantity(slot).orders > 0) {
        emit_countdown(slot);
      }
    }
    const std::uint64_t every = std::uint64_t{countdown_ms()} * 1'000'000U / 4U;
    k_.countdown.armed = core::ok(
        k_.timers.schedule(core::UnixNanos{k_.current.ts.value() + every}, core::DurationNanos{},
                           core::TimerKey{strategy::kKernelTimerOwner, strategy::kCountdownTimerId},
                           k_.countdown.timer));
  }

  // Stopped with nothing left open (or the orders kept on purpose): countdown 0 for every
  // instrument renewed since the last renewal. Others have no open orders; their countdown, if
  // still running, cancels nothing.
  void disarm_countdown() {
    if (k_.countdown.armed) {
      static_cast<void>(k_.timers.cancel(k_.countdown.timer));
      k_.countdown.armed = false;
    }
    for (std::uint32_t slot = 0; slot < k_.instruments.size() && slot < k_.countdown.live.size();
         ++slot) {
      if (k_.countdown.live[slot] == 0) {
        continue;
      }
      model::CountdownCancelAll c;
      c.instrument_id = k_.instruments.id(model::InstrumentSlot{slot});
      c.countdown_ms = 0;
      c.ts_init = k_.current.ts;
      if (core::ok(k_.outputs.emplace_back(std::in_place_type<model::CountdownCancelAll>, c))) {
        k_.countdown.live[slot] = 0;
      }
    }
  }

  void cover_submits(std::size_t first) {
    if (countdown_ms() == 0 || !started_) {
      return;
    }
    const std::size_t end = k_.outputs.size();
    for (std::size_t i = first; i < end; ++i) {
      const auto* c = std::get_if<model::SubmitOrder>(&k_.outputs[i]);
      model::InstrumentSlot slot;
      if (c != nullptr && core::ok(k_.instruments.find(c->instrument_id, slot)) &&
          slot.value < k_.countdown.live.size() && k_.countdown.live[slot.value] == 0) {
        emit_countdown(slot.value);
      }
    }
  }

  // An operator's parameter for one strategy; a strategy that has not started, has stopped or
  // was halted does not get it (the input is still recorded).
  core::Status on_param_update(const model::ParamUpdate& e) {
    if (e.strategy_index >= ss_->size()) {
      return core::Status::InvalidArgument;
    }
    const auto s = static_cast<StrategyIndex>(e.strategy_index);
    if (!started_ || k_.stopped || k_.is_disabled(s)) {
      return core::Status::Ok;
    }
    strategy::Context ctx{k_, s};
    const core::Status status = ss_->on_params_changed(s, ctx, e);
    if (!core::ok(status)) {
      k_.fail(s, status);
    }
    return core::Status::Ok;
  }

  core::Status on_strategy_error(const model::StrategyError& e) {
    if (e.strategy_index >= ss_->size()) {
      return core::Status::InvalidArgument;
    }
    const auto s = static_cast<StrategyIndex>(e.strategy_index);
    if (k_.is_disabled(s)) {
      return core::Status::Ok;
    }
    strategy::Context ctx{k_, s};
    static_cast<void>(ss_->on_error(s, ctx, e));
    switch (policy_) {
    case strategy::ErrorPolicy::HaltStrategy:
      halt_strategy(s);
      break;
    case strategy::ErrorPolicy::HaltNode:
      halt_strategy(s);
      k_.halt_requested = true;
      break;
    case strategy::ErrorPolicy::Ignore:
      break;
    }
    return core::Status::Ok;
  }

  // ---- market data freshness (section 19.3) ----

  void note_market_data() noexcept {
    k_.freshness.last_market_data = k_.current.ts;
    k_.health.market_data_stale = false; // fresh again: the sync gate brings the node back
  }

  // A periodic check at half the limit, once market data has been up (never in a backtest).
  void arm_freshness() noexcept {
    const std::uint64_t limit = k_.config().market_data_stale_ns;
    if (limit == 0 || k_.freshness.armed) {
      return;
    }
    const std::uint64_t every = limit / 2 > 0 ? limit / 2 : 1;
    k_.freshness.armed = core::ok(k_.timers.schedule(
        core::UnixNanos{k_.current.ts.value() + every}, core::DurationNanos{every},
        core::TimerKey{strategy::kKernelTimerOwner, strategy::kStaleTimerId}, k_.freshness.timer));
  }

  void check_freshness() noexcept {
    const std::uint64_t limit = k_.config().market_data_stale_ns;
    const std::uint64_t now = k_.current.ts.value();
    const std::uint64_t last = k_.freshness.last_market_data.value();
    if (limit == 0 || k_.health.market_data != execution::LinkState::Up ||
        k_.health.market_data_stale || now < last || now - last <= limit) {
      return;
    }
    k_.health.market_data_stale = true; // the sync gate moves Running to Degraded
    strategy::LogRecord r{strategy::LogCode::MarketDataStale, strategy::kNoLogStrategy, {}};
    r.args[0] = static_cast<std::int64_t>(now - last);
    r.args[1] = static_cast<std::int64_t>(limit);
    k_.log(r);
  }

  // A halted strategy receives nothing more; its open orders are canceled (section 7.6).
  void halt_strategy(StrategyIndex s) {
    k_.disabled[s] = 1;
    std::uint32_t canceled = 0;
    static_cast<void>(k_.cancel_all(s, nullptr, canceled));
    strategy::LogRecord r{strategy::LogCode::StrategyHalted, s, {}};
    r.args[0] = policy_ == strategy::ErrorPolicy::HaltNode ? 1 : 0;
    k_.log(r);
  }

  // What the step's log records compare before and after it.
  struct Watched {
    model::TradingState state = model::TradingState::Active;
    model::TradingState base = model::TradingState::Active;
    bool syncing = false;
    bool degraded = false;
    std::uint64_t kill_switches = 0;
  };

  [[nodiscard]] Watched watched() const noexcept {
    const risk::TradingStateMachine& t = k_.trading.risk.state_machine();
    return Watched{t.state(), t.base(), t.syncing(), t.degraded(),
                   k_.trading.risk.stats().kill_switches};
  }

  void log_changes(const Watched& before, std::size_t first_output) noexcept {
    const Watched after = watched();
    if (after.kill_switches != before.kill_switches) {
      std::int64_t cancels = 0;
      for (std::size_t i = first_output; i < k_.outputs.size(); ++i) {
        cancels += std::holds_alternative<model::CancelOrder>(k_.outputs[i]) ? 1 : 0;
      }
      strategy::LogRecord r{strategy::LogCode::KillSwitch, strategy::kNoLogStrategy, {}};
      r.args[0] = cancels;
      k_.log(r);
    }
    if (after.state != before.state || after.base != before.base ||
        after.syncing != before.syncing || after.degraded != before.degraded) {
      strategy::LogRecord r{strategy::LogCode::TradingStateChanged, strategy::kNoLogStrategy, {}};
      r.args = {static_cast<std::int64_t>(before.state), static_cast<std::int64_t>(after.state),
                static_cast<std::int64_t>(after.base),
                (after.syncing ? 1 : 0) + (after.degraded ? 2 : 0)};
      k_.log(r);
    }
  }

  KernelServices k_;
  SS* ss_;
  strategy::ErrorPolicy policy_;
  core::FixedVector<StrategyIndex> calls_;
  core::FixedVector<std::uint32_t> overflowing_;
  bool started_ = false;
};

} // namespace jarvis::engine
