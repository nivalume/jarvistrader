#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/execution/execution_engine.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/execution/order_intent.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/risk/order_checks.hpp"

// The kernel's trading state and command path (docs/architecture.md sections 8 and 9):
// instrument definitions, the OMS, the identities orders are issued under, and the order events
// the kernel itself produced in this step.
//
//   submit   assigns the ClientOrderId, checks the intent, then either denies it (OrderDenied
//            output and event) or marks it SUBMITTED (SubmitOrder output, OrderSubmitted event);
//   modify   PENDING_UPDATE, ModifyOrder output, OrderPendingUpdate event;
//   cancel   PENDING_CANCEL, CancelOrder output, OrderPendingCancel event.
//
// A command either happens completely or not at all: the capacity it needs (one output, one
// event) is checked before anything changes. The events wait in `events` until the callback that
// caused them returns; the engine then delivers them to on_order_event. Venue events (accepted,
// filled, canceled, ...) are applied by on_venue_event and delivered by the engine directly.
// Denials are events, not errors: submit returns Ok for a denied order. Functions that read an
// instrument definition go through std::visit and are not noexcept (bad_variant_access cannot
// happen for these types).

namespace jarvis::strategy {

using data::StrategyIndex;
using execution::OrderIntent;

struct TradingConfig {
  std::uint32_t orders = 4096;       // live and recently closed orders
  std::uint32_t trades = 65536;      // fill records for duplicate detection
  std::uint32_t order_events = 1024; // kernel-produced order events per step
  model::TraderId trader_id;         // default JARVIS-001
  model::AccountId account_id;       // default SIM-001
  core::FixedString<8> node_tag;     // ClientOrderId prefix; default "jarvis"
  std::uint64_t epoch = 1;           // ClientOrderId epoch (always 1 in backtest)
};

struct PendingOrderEvent {
  StrategyIndex strategy = 0;
  model::OrderEvent event;
};

struct TradingStats {
  std::uint64_t submitted = 0;
  std::uint64_t denied = 0;
  std::uint64_t modifies = 0;
  std::uint64_t cancels = 0;
  std::uint64_t venue_events = 0;         // applied venue order events
  std::uint64_t unknown_order_events = 0; // venue events for orders the OMS does not know
  std::uint64_t refused_order_events = 0; // transitions or quantities the OMS refused
  std::uint64_t duplicate_fills = 0;
};

// A copy of one order, for strategies (ctx.order, ctx.open_orders).
struct OrderView {
  model::ClientOrderId client_order_id;
  std::optional<model::VenueOrderId> venue_order_id;
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::Buy;
  model::OrderType type = model::OrderType::Limit;
  model::TimeInForce time_in_force = model::TimeInForce::Gtc;
  bool post_only = false;
  bool reduce_only = false;
  std::optional<model::Price> price;
  model::OrderStatus status = model::OrderStatus::Initialized;
  model::Quantity quantity;
  model::Quantity filled;
  model::Quantity leaves;
  std::optional<model::Price> avg_px; // truncated to the instrument's price precision
  core::UnixNanos ts_init;
};

using Outputs = core::FixedVector<model::Output>;

namespace detail {

template <typename Id> Id id_or(std::string_view text, const Id& given) {
  if (!given.empty()) {
    return given;
  }
  Id id;
  static_cast<void>(Id::from(text, id));
  return id;
}

// "strategy-001", "strategy-002", ...
inline model::StrategyId default_strategy_id(std::uint32_t index) {
  std::array<char, 16> text = {'s', 't', 'r', 'a', 't', 'e', 'g', 'y', '-'};
  const std::uint32_t n = index + 1;
  text[9] = static_cast<char>('0' + (n / 100) % 10);
  text[10] = static_cast<char>('0' + (n / 10) % 10);
  text[11] = static_cast<char>('0' + n % 10);
  model::StrategyId id;
  static_cast<void>(model::StrategyId::from(std::string_view{text.data(), 12}, id));
  return id;
}

// Event ids draw from their own Philox key, apart from the strategies' ctx.rng streams.
inline constexpr std::uint64_t kEventIdSalt = 0x6A09E667F3BCC909ULL;

} // namespace detail

class Trading {
public:
  Trading(const TradingConfig& c, std::uint32_t instruments, std::uint32_t strategies,
          std::uint64_t seed)
      : oms{c.orders, c.trades}, definitions{instruments},
        trader_id{detail::id_or<model::TraderId>("JARVIS-001", c.trader_id)},
        account_id{detail::id_or<model::AccountId>("SIM-001", c.account_id)},
        strategy_ids{strategies}, events{c.order_events}, event_rng_{seed ^ detail::kEventIdSalt} {
    for (std::uint32_t i = 0; i < instruments; ++i) {
      static_cast<void>(definitions.push_back(std::nullopt));
    }
    for (std::uint32_t i = 0; i < strategies; ++i) {
      static_cast<void>(strategy_ids.push_back(detail::default_strategy_id(i)));
    }
    const std::string_view tag =
        c.node_tag.empty() ? std::string_view{"jarvis"} : c.node_tag.view();
    if (!core::ok(model::ClientOrderIdGenerator::create(tag, c.epoch, ids))) {
      static_cast<void>(model::ClientOrderIdGenerator::create("jarvis", 1, ids));
    }
  }

  // Called by the engine before each input.
  void begin_step() noexcept { event_serial_ = 0; }

  // ---- instrument definitions ---------------------------------------------------------------

  [[nodiscard]] const model::Instrument* definition(std::uint32_t slot) const noexcept {
    if (slot >= definitions.size()) {
      return nullptr;
    }
    const std::optional<model::Instrument>& def = definitions[slot];
    return def.has_value() ? &*def : nullptr;
  }

  // ---- commands -----------------------------------------------------------------------------

  // `slot` is the instrument's slot, or kNoIndex when the kernel has never seen it.
  [[nodiscard]] core::Status submit(const core::EventKey& now, StrategyIndex s,
                                    const OrderIntent& intent, std::uint32_t slot, Outputs& outputs,
                                    model::ClientOrderId& cid) {
    if (!room(outputs)) {
      return core::Status::CapacityExceeded;
    }
    const core::Status id = ids.next(cid);
    if (!core::ok(id)) {
      return id;
    }
    execution::OrderRecord record;
    record.client_order_id = cid;
    record.strategy = s;
    record.slot = slot;
    record.instrument_id = intent.instrument_id;
    record.side = intent.side;
    record.type = intent.type;
    record.time_in_force = intent.time_in_force;
    record.post_only = intent.post_only;
    record.reduce_only = intent.reduce_only;
    record.price = intent.price;
    record.state = execution::OrderState{intent.quantity};
    record.ts_init = now.ts;

    const model::Instrument* def = definition(slot);
    std::string_view reason = def == nullptr
                                  ? risk::reason::kInstrumentUnknown
                                  : risk::check_intent(model::common(*def), intent, now.ts);
    std::uint32_t index = execution::kNoIndex;
    if (!core::ok(oms.create(record, index))) {
      index = execution::kNoIndex;
      if (reason.empty()) {
        reason = risk::reason::kOmsCapacityExceeded;
      }
    }
    if (!reason.empty()) {
      deny(now, s, record, index, reason, outputs);
      return core::Status::Ok;
    }
    static_cast<void>(oms.apply(index, execution::OrderEventKind::Submitted));
    model::SubmitOrder command;
    command.strategy_index = s;
    command.client_order_id = cid;
    command.instrument_id = intent.instrument_id;
    command.order_side = intent.side;
    command.order_type = intent.type;
    command.quantity = intent.quantity;
    command.price = intent.price;
    command.time_in_force = intent.time_in_force;
    command.post_only = intent.post_only;
    command.reduce_only = intent.reduce_only;
    command.expire_time = intent.expire_time;
    command.ts_init = now.ts;
    static_cast<void>(outputs.emplace_back(std::in_place_type<model::SubmitOrder>, command));
    model::OrderSubmitted event;
    event.header = header(now, s, record);
    event.account_id = account_id;
    static_cast<void>(events.push_back(PendingOrderEvent{s, model::OrderEvent{event}}));
    ++stats.submitted;
    return core::Status::Ok;
  }

  // A LIMIT order's new quantity and/or price. NotFound for an order this strategy does not
  // own; InvalidState when the order cannot be modified now (closed, or a cancel is pending).
  [[nodiscard]] core::Status modify(const core::EventKey& now, StrategyIndex s,
                                    const model::ClientOrderId& cid,
                                    std::optional<model::Quantity> quantity,
                                    std::optional<model::Price> price, Outputs& outputs) {
    const std::uint32_t index = owned(s, cid);
    if (index == execution::kNoIndex) {
      return core::Status::NotFound;
    }
    execution::OrderRecord& r = oms.at(index);
    const model::Instrument* def = definition(r.slot);
    if (r.type != model::OrderType::Limit || !r.price || def == nullptr || (!quantity && !price)) {
      return core::Status::InvalidArgument;
    }
    const model::Quantity q = quantity.value_or(r.state.quantity());
    const model::Price p = price.value_or(*r.price);
    if (!risk::check_price(model::common(*def), p).empty() ||
        q.precision() != r.state.quantity().precision() || q.raw() <= r.state.filled().raw()) {
      return core::Status::InvalidArgument;
    }
    if (!can(r, execution::OrderEventKind::PendingUpdate)) {
      return core::Status::InvalidState;
    }
    if (!room(outputs)) {
      return core::Status::CapacityExceeded;
    }
    static_cast<void>(oms.apply(index, execution::OrderEventKind::PendingUpdate));
    static_cast<void>(outputs.emplace_back(
        std::in_place_type<model::ModifyOrder>,
        model::ModifyOrder{s, r.client_order_id, r.instrument_id, r.venue_order_id, q, p, now.ts}));
    model::OrderPendingUpdate event;
    event.header = header(now, s, r);
    event.account_id = account_id;
    event.venue_order_id = r.venue_order_id;
    static_cast<void>(events.push_back(PendingOrderEvent{s, model::OrderEvent{event}}));
    ++stats.modifies;
    return core::Status::Ok;
  }

  // NotFound for an order this strategy does not own; InvalidState when it is closed or a
  // cancel is already pending.
  [[nodiscard]] core::Status cancel(const core::EventKey& now, StrategyIndex s,
                                    const model::ClientOrderId& cid, Outputs& outputs) noexcept {
    const std::uint32_t index = owned(s, cid);
    if (index == execution::kNoIndex) {
      return core::Status::NotFound;
    }
    return cancel_index(now, s, index, outputs);
  }

  // Cancels every cancelable order of the strategy (of one instrument when `id` is given);
  // `canceled` counts the cancels sent.
  [[nodiscard]] core::Status cancel_all(const core::EventKey& now, StrategyIndex s,
                                        const model::InstrumentId* id, Outputs& outputs,
                                        std::uint32_t& canceled) noexcept {
    canceled = 0;
    for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
      const execution::OrderRecord& r = oms.at(i);
      if (!r.used || r.strategy != s || (id != nullptr && !(r.instrument_id == *id)) ||
          !can(r, execution::OrderEventKind::PendingCancel)) {
        continue;
      }
      const core::Status st = cancel_index(now, s, i, outputs);
      if (!core::ok(st)) {
        return st;
      }
      ++canceled;
    }
    return core::Status::Ok;
  }

  // ---- venue events -------------------------------------------------------------------------

  // Applies an order event from the venue; true, with the owning strategy, when it was applied
  // and the strategy should receive it.
  [[nodiscard]] bool on_venue_event(const model::OrderEvent& event, StrategyIndex& owner) {
    std::uint32_t index = execution::kNoIndex;
    switch (execution::apply_order_event(oms, event, index)) {
    case execution::EventOutcome::Applied:
      ++stats.venue_events;
      owner = oms.at(index).strategy;
      return true;
    case execution::EventOutcome::UnknownOrder:
      ++stats.unknown_order_events;
      return false;
    case execution::EventOutcome::DuplicateFill:
      ++stats.duplicate_fills;
      return false;
    case execution::EventOutcome::Refused:
      ++stats.refused_order_events;
      return false;
    }
    return false;
  }

  // ---- queries ------------------------------------------------------------------------------

  [[nodiscard]] bool order(StrategyIndex s, const model::ClientOrderId& cid, OrderView& out) const {
    const std::uint32_t index = oms.find(cid);
    if (index == execution::kNoIndex || oms.at(index).strategy != s) {
      return false;
    }
    out = view(oms.at(index));
    return true;
  }

  // The strategy's open orders (of one instrument when `id` is given), oldest slot first: writes
  // up to out.size() of them and returns how many there are.
  [[nodiscard]] std::size_t open_orders(StrategyIndex s, const model::InstrumentId* id,
                                        std::span<OrderView> out) const {
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < oms.used_bound(); ++i) {
      const execution::OrderRecord& r = oms.at(i);
      if (!r.used || r.strategy != s || !execution::is_open(r.state.status()) ||
          (id != nullptr && !(r.instrument_id == *id))) {
        continue;
      }
      if (n < out.size()) {
        out[n] = view(r);
      }
      ++n;
    }
    return n;
  }

  // ---- state --------------------------------------------------------------------------------

  execution::Oms oms;
  core::FixedVector<std::optional<model::Instrument>> definitions; // by instrument slot
  model::ClientOrderIdGenerator ids;
  model::TraderId trader_id;
  model::AccountId account_id;
  core::FixedVector<model::StrategyId> strategy_ids;
  core::FixedVector<PendingOrderEvent> events; // kernel-produced, delivered after the callback
  TradingStats stats;

private:
  [[nodiscard]] bool room(const Outputs& outputs) const noexcept {
    return outputs.size() < outputs.capacity() && events.size() < events.capacity();
  }

  [[nodiscard]] static bool can(const execution::OrderRecord& r,
                                execution::OrderEventKind kind) noexcept {
    const model::OrderStatus from = r.state.status();
    if (kind == execution::OrderEventKind::PendingCancel &&
        from == model::OrderStatus::PendingCancel) {
      return false; // one cancel in flight is enough
    }
    model::OrderStatus to = from;
    return execution::next_status(from, kind, to);
  }

  [[nodiscard]] std::uint32_t owned(StrategyIndex s,
                                    const model::ClientOrderId& cid) const noexcept {
    const std::uint32_t index = oms.find(cid);
    return index != execution::kNoIndex && oms.at(index).strategy == s ? index
                                                                       : execution::kNoIndex;
  }

  [[nodiscard]] core::Status cancel_index(const core::EventKey& now, StrategyIndex s,
                                          std::uint32_t index, Outputs& outputs) noexcept {
    execution::OrderRecord& r = oms.at(index);
    if (!can(r, execution::OrderEventKind::PendingCancel)) {
      return core::Status::InvalidState;
    }
    if (!room(outputs)) {
      return core::Status::CapacityExceeded;
    }
    static_cast<void>(oms.apply(index, execution::OrderEventKind::PendingCancel));
    static_cast<void>(outputs.emplace_back(
        std::in_place_type<model::CancelOrder>,
        model::CancelOrder{s, r.client_order_id, r.instrument_id, r.venue_order_id, now.ts}));
    model::OrderPendingCancel event;
    event.header = header(now, s, r);
    event.account_id = account_id;
    event.venue_order_id = r.venue_order_id;
    static_cast<void>(events.push_back(PendingOrderEvent{s, model::OrderEvent{event}}));
    ++stats.cancels;
    return core::Status::Ok;
  }

  // `index` is kNoIndex when the OMS had no room for the order.
  void deny(const core::EventKey& now, StrategyIndex s, const execution::OrderRecord& record,
            std::uint32_t index, std::string_view reason, Outputs& outputs) noexcept {
    if (index != execution::kNoIndex) {
      static_cast<void>(oms.apply(index, execution::OrderEventKind::Denied));
    }
    model::OrderDenied denied;
    denied.header = header(now, s, record);
    static_cast<void>(model::ReasonText::from(reason, denied.reason));
    static_cast<void>(outputs.emplace_back(std::in_place_type<model::OrderDenied>, denied));
    static_cast<void>(events.push_back(PendingOrderEvent{s, model::OrderEvent{denied}}));
    ++stats.denied;
  }

  [[nodiscard]] model::OrderEventHeader header(const core::EventKey& now, StrategyIndex s,
                                               const execution::OrderRecord& r) noexcept {
    model::OrderEventHeader h;
    h.trader_id = trader_id;
    if (s < strategy_ids.size()) {
      h.strategy_id = strategy_ids[s];
    }
    h.instrument_id = r.instrument_id;
    h.client_order_id = r.client_order_id;
    h.event_id = model::Uuid4::derive(event_rng_, now.seq, event_serial_++);
    h.ts_event = now.ts;
    h.ts_init = now.ts;
    return h;
  }

  [[nodiscard]] OrderView view(const execution::OrderRecord& r) const {
    OrderView v;
    v.client_order_id = r.client_order_id;
    v.venue_order_id = r.venue_order_id;
    v.instrument_id = r.instrument_id;
    v.side = r.side;
    v.type = r.type;
    v.time_in_force = r.time_in_force;
    v.post_only = r.post_only;
    v.reduce_only = r.reduce_only;
    v.price = r.price;
    v.status = r.state.status();
    v.quantity = r.state.quantity();
    v.filled = r.state.filled();
    v.leaves = r.state.leaves();
    v.ts_init = r.ts_init;
    const model::Instrument* def = definition(r.slot);
    model::Price avg;
    if (def != nullptr &&
        execution::Oms::average_price(r, model::common(*def).price_precision, avg)) {
      v.avg_px = avg;
    }
    return v;
  }

  core::CounterRng event_rng_;
  std::uint32_t event_serial_ = 0;
};

} // namespace jarvis::strategy
