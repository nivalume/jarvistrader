#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

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
#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/position_events.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/portfolio/margin.hpp"
#include "jarvis/portfolio/portfolio.hpp"
#include "jarvis/risk/gates.hpp"
#include "jarvis/risk/order_checks.hpp"
#include "jarvis/risk/trading_state.hpp"
#include "jarvis/strategy/exec_algo.hpp"

// The kernel's trading state and command path (docs/architecture.md sections 8 and 9):
// instrument definitions, the OMS, the identities orders are issued under, and the order events
// the kernel itself produced in this step.
//
//   submit   assigns the ClientOrderId, checks the intent, then either denies it (OrderDenied
//            output and event) or marks it SUBMITTED (SubmitOrder output, OrderSubmitted event);
//   modify   PENDING_UPDATE, ModifyOrder output, OrderPendingUpdate event;
//   cancel   PENDING_CANCEL, CancelOrder output, OrderPendingCancel event.
//   submit_parent  checks the parent intent against Gate A (an OrderDenied names the parent when
//            it fails), then hands it to its execution algorithm, whose children are submitted
//            like orders but pass Gate B only (exec_algo.hpp).
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
  std::uint32_t currencies = 16;     // balances the portfolio tracks
  std::uint32_t parents = 256;       // execution algorithm parents working at once
  portfolio::Margin margin = portfolio::StandardMargin{};
  risk::RiskConfig risk;
  execution::ReconcileConfig reconcile;
};

// An event the kernel produced for a strategy in this step.
using KernelEvent = std::variant<model::OrderEvent, model::PositionEvent>;

struct PendingEvent {
  StrategyIndex strategy = 0;
  KernelEvent event;

  template <typename Ar> void state(Ar& ar) { ar(strategy, event); }
};

struct TradingStats {
  std::uint64_t submitted = 0;
  std::uint64_t denied = 0;
  std::uint64_t modifies = 0;
  std::uint64_t cancels = 0;
  std::uint64_t venue_events = 0;         // applied venue order events
  std::uint64_t unknown_order_events = 0; // venue events for orders the OMS does not know
  std::uint64_t refused_order_events = 0; // transitions or quantities the OMS refused
  std::uint64_t stale_order_events = 0;   // status events older than the order's latest
  std::uint64_t duplicate_fills = 0;
  std::uint64_t dropped_events = 0; // kernel events lost to a full queue (position events)
  std::uint64_t parents = 0;        // parents accepted by Gate A
  std::uint64_t children = 0;       // child orders submitted by execution algorithms

  template <typename Ar> void state(Ar& ar) {
    ar(submitted, denied, modifies, cancels, venue_events, unknown_order_events,
       refused_order_events, stale_order_events, duplicate_fills, dropped_events, parents,
       children);
  }
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
  std::optional<model::Price> avg_px;            // truncated to the instrument's price precision
  std::optional<model::ClientOrderId> parent_id; // the execution algorithm's parent, if any
  core::UnixNanos ts_init;
};

// A strategy's position in one instrument (its share of the venue position, from its own
// fills). PnL amounts are in the settlement currency; realized_pnl is net of commissions and
// funding since the position opened.
struct PositionView {
  model::InstrumentId instrument_id;
  model::PositionId position_id;
  model::PositionSide side = model::PositionSide::Flat;
  model::Decimal signed_qty;
  model::Quantity quantity;
  std::optional<model::Price> avg_px_open; // full precision
  model::Money realized_pnl;
  model::Money unrealized_pnl; // at the mark price, else the last trade
  model::Money commission;
  model::Money funding;
  model::Money total_pnl; // realized, net of commissions and funding, since the node started
  core::UnixNanos ts_opened;
};

// open_exposure() of one instrument (section 9.3): the venue position (all strategies) plus the
// leaves of open orders and the parents' remaining quantity that no child works yet.
struct ExposureView {
  model::InstrumentId instrument_id;
  model::Decimal position;              // signed, raw at the instrument's size precision
  model::Quantity open_buy;             // leaves of open buy orders and reserved buy parents
  model::Quantity open_sell;            // the same for sells
  model::Decimal max_long;              // position + open buys
  model::Decimal max_short;             // position - open sells
  std::optional<model::Money> notional; // the larger of |max_long| and |max_short| at the mark
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
      : oms{c.orders, c.trades, instruments}, definitions{instruments},
        trader_id{detail::id_or<model::TraderId>("JARVIS-001", c.trader_id)},
        account_id{detail::id_or<model::AccountId>("SIM-001", c.account_id)},
        strategy_ids{strategies}, events{c.order_events},
        portfolio{portfolio::PortfolioConfig{instruments, strategies, c.currencies, c.margin}},
        risk{c.risk, instruments, strategies}, algos{c.parents, instruments},
        reconciler{c.reconcile, c.orders, c.currencies}, tops{instruments},
        event_rng_{seed ^ detail::kEventIdSalt} {
    for (std::uint32_t i = 0; i < instruments; ++i) {
      static_cast<void>(definitions.push_back(std::nullopt));
      static_cast<void>(tops.push_back(std::nullopt));
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
    bool denied = false;
    return place(now, s, intent, slot, execution::kNoIndex, outputs, cid, denied);
  }

  // A parent order for execution algorithm `kind`. Ok when the parent was denied (an
  // OrderDenied names it) as for submit; InvalidArgument for an unknown instrument slot.
  [[nodiscard]] core::Status submit_parent(const core::EventKey& now, StrategyIndex s,
                                           AlgoKind kind, const AlgoParams& params,
                                           const OrderIntent& intent, std::uint32_t slot,
                                           Outputs& outputs, model::ClientOrderId& parent_id) {
    if (!room(outputs)) {
      return core::Status::CapacityExceeded;
    }
    const core::Status id = ids.next(parent_id);
    if (!core::ok(id)) {
      return id;
    }
    execution::OrderRecord record; // only for the denial's header
    record.client_order_id = parent_id;
    record.instrument_id = intent.instrument_id;
    const model::Instrument* def = definition(slot);
    std::string_view reason = def == nullptr
                                  ? risk::reason::kInstrumentUnknown
                                  : risk::check_intent(model::common(*def), intent, now.ts);
    if (reason.empty()) {
      const risk::OrderCheck check =
          order_check(now, s, slot, model::common(*def), intent.side, intent.type, intent.quantity,
                      intent.price, intent.reduce_only, std::nullopt);
      reason = risk.check_parent(check);
    }
    AlgoState parent;
    parent.parent_id = parent_id;
    parent.parent_seq = ids.last_seq();
    parent.intent = intent;
    parent.kind = kind;
    parent.params = params;
    parent.strategy = s;
    parent.slot = slot;
    std::uint32_t index = execution::kNoIndex;
    if (reason.empty() && !core::ok(algos.open(parent, index))) {
      reason = kParentCapacityExceeded;
    }
    if (!reason.empty()) {
      deny(now, s, record, execution::kNoIndex, reason, outputs);
      return core::Status::Ok;
    }
    ++stats.parents;
    AlgoContext ctx{*this, now, outputs, index};
    const core::Status st =
        dispatch(kind, [&](const auto& algo) { return algo.on_parent(algos.at(index), ctx); });
    static_cast<void>(algos.settle(index));
    return st;
  }

  static constexpr std::string_view kParentCapacityExceeded = "ALGO_CAPACITY_EXCEEDED";

  // What an execution algorithm may do while it works one parent (the Ctx of ExecAlgorithm).
  class AlgoContext {
  public:
    AlgoContext(Trading& t, const core::EventKey& now, Outputs& outputs, std::uint32_t parent)
        : t_{t}, now_{now}, outputs_{outputs}, parent_{parent} {}

    [[nodiscard]] core::UnixNanos now() const noexcept { return now_.ts; }
    [[nodiscard]] const AlgoState& parent() const noexcept { return t_.algos.at(parent_); }
    // Orders the rate windows still allow now.
    [[nodiscard]] std::uint32_t rate_budget() noexcept {
      return t_.risk.limiter().remaining(now_.ts);
    }

    // A child on the parent's instrument and side for at most the remaining quantity that no
    // child works yet; it passes Gate B and the rate limit. `denied` tells whether it failed
    // them (an OrderDenied names the child); InvalidArgument for a child that does not fit the
    // parent, CapacityExceeded with kAlgoChildren children working.
    [[nodiscard]] core::Status submit(const OrderIntent& child, model::ClientOrderId& out,
                                      bool& denied) {
      const AlgoState& p = parent();
      denied = false;
      if (!(child.instrument_id == p.intent.instrument_id) || child.side != p.intent.side ||
          child.quantity.raw() == 0 || child.quantity.raw() > p.reserved_raw) {
        return core::Status::InvalidArgument;
      }
      if (p.child_count >= kAlgoChildren) {
        return core::Status::CapacityExceeded;
      }
      const core::Status s =
          t_.place(now_, p.strategy, child, p.slot, parent_, outputs_, out, denied);
      if (core::ok(s) && !denied) {
        ++t_.stats.children;
        return t_.algos.add_child(parent_, out, child.quantity.raw());
      }
      return s;
    }
    [[nodiscard]] core::Status modify(const model::ClientOrderId& child,
                                      std::optional<model::Quantity> quantity,
                                      std::optional<model::Price> price) {
      return t_.modify(now_, parent().strategy, child, quantity, price, outputs_);
    }
    [[nodiscard]] core::Status cancel(const model::ClientOrderId& child) noexcept {
      return t_.cancel_child(now_, parent().strategy, child, outputs_);
    }
    // Cancels every working child that a cancel can still reach.
    [[nodiscard]] core::Status cancel_children() noexcept {
      const AlgoState& p = parent();
      for (std::size_t i = 0; i < p.child_count; ++i) {
        const core::Status s = t_.cancel_child(now_, p.strategy, p.children[i].id, outputs_);
        if (!core::ok(s) && s != core::Status::InvalidState) {
          return s;
        }
      }
      return core::Status::Ok;
    }
    // The algorithm has nothing more to do: the parent closes once no child works.
    void finish() noexcept { t_.algos.finish(parent_); }

    // Whether a modify or cancel of `child` is still unanswered.
    [[nodiscard]] bool pending(const model::ClientOrderId& child) const noexcept {
      const std::uint32_t index = t_.oms.find(child);
      if (index == execution::kNoIndex) {
        return false;
      }
      const model::OrderStatus s = t_.oms.at(index).state.status();
      return s == model::OrderStatus::PendingUpdate || s == model::OrderStatus::PendingCancel;
    }
    // Whether the venue has acknowledged `child` (it has the venue's order id).
    [[nodiscard]] bool acknowledged(const model::ClientOrderId& child) const noexcept {
      const std::uint32_t index = t_.oms.find(child);
      return index != execution::kNoIndex && t_.oms.at(index).venue_order_id.has_value();
    }
    // The instrument's best bid and ask, once known (quotes or the L2 book).
    [[nodiscard]] std::optional<AlgoTop> top() const noexcept {
      const std::uint32_t slot = parent().slot;
      return slot < t_.tops.size() ? t_.tops[slot] : std::nullopt;
    }
    // The instrument's price and size increments.
    [[nodiscard]] std::optional<model::Price> tick() const noexcept {
      const model::Instrument* def = t_.definition(parent().slot);
      return def != nullptr ? std::optional{model::common(*def).price_increment} : std::nullopt;
    }
    [[nodiscard]] std::optional<model::Quantity> lot() const noexcept {
      const model::Instrument* def = t_.definition(parent().slot);
      return def != nullptr ? std::optional{model::common(*def).size_increment} : std::nullopt;
    }
    // An AlgoTimer at `deadline` (one per parent; a later call replaces it), or none.
    void wake_at(core::UnixNanos deadline) noexcept {
      t_.algos.at(parent_).wake_ns = deadline.value();
      t_.algo_wake_changed = true;
    }
    void sleep() noexcept {
      t_.algos.at(parent_).wake_ns = 0;
      t_.algo_wake_changed = true;
    }

  private:
    Trading& t_;
    const core::EventKey& now_;
    Outputs& outputs_;
    std::uint32_t parent_;
  };

  // Cancels parent `index`: its algorithm cancels what it has working.
  [[nodiscard]] core::Status cancel_parent(const core::EventKey& now, std::uint32_t index,
                                           Outputs& outputs) {
    AlgoState& p = algos.at(index);
    if (p.canceling) {
      return core::Status::InvalidState;
    }
    algos.cancel(index);
    AlgoContext ctx{*this, now, outputs, index};
    const core::Status st =
        dispatch(p.kind, [&](const auto& algo) { return algo.on_cancel(algos.at(index), ctx); });
    static_cast<void>(algos.settle(index));
    return st;
  }

  // The strategy's parent `id`; false when it has none of that id working.
  [[nodiscard]] bool parent(StrategyIndex s, const model::ClientOrderId& id,
                            ParentView& out) const {
    const std::uint32_t index = algos.find(s, id);
    if (index == execution::kNoIndex) {
      return false;
    }
    const AlgoState& p = algos.at(index);
    const std::uint8_t precision = p.intent.quantity.precision();
    out = ParentView{};
    out.parent_id = p.parent_id;
    out.instrument_id = p.intent.instrument_id;
    out.side = p.intent.side;
    out.kind = p.kind;
    out.quantity = p.intent.quantity;
    static_cast<void>(model::Quantity::from_raw(p.filled_raw, precision, out.filled));
    static_cast<void>(model::Quantity::from_raw(p.reserved_raw, precision, out.reserved));
    out.children = p.child_count;
    out.active = p.active;
    out.canceling = p.canceling;
    return true;
  }

  // An order of strategy `s`, or of its execution algorithm when `parent` is a parent slot. For
  // a strategy order the intent passes both gates; for a child Gate B only. `denied` tells
  // whether it was denied (an OrderDenied names it).
  [[nodiscard]] core::Status place(const core::EventKey& now, StrategyIndex s,
                                   const OrderIntent& intent, std::uint32_t slot,
                                   std::uint32_t parent, Outputs& outputs,
                                   model::ClientOrderId& cid, bool& denied) {
    denied = false;
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
    record.parent = parent;
    if (parent != execution::kNoIndex) {
      record.parent_seq = algos.at(parent).parent_seq;
    }

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
    if (reason.empty()) {
      const risk::OrderCheck check =
          order_check(now, s, slot, model::common(*def), intent.side, intent.type, intent.quantity,
                      intent.price, intent.reduce_only, std::nullopt);
      reason = parent == execution::kNoIndex ? risk.check_order(check) : risk.check_child(check);
    }
    if (!reason.empty()) {
      deny(now, s, record, index, reason, outputs);
      denied = true;
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
    static_cast<void>(events.push_back(PendingEvent{s, KernelEvent{model::OrderEvent{event}}}));
    ++stats.submitted;
    return core::Status::Ok;
  }

  // A LIMIT order's new quantity and/or price. NotFound for an order this strategy does not
  // own; InvalidState when the order cannot be modified now (closed, or a cancel is pending).
  // A modify the risk gate refuses is not an error: OrderModifyRejected arrives as an event and
  // the order stays as it was.
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
    const risk::OrderCheck check = order_check(now, s, r.slot, model::common(*def), r.side, r.type,
                                               q, p, r.reduce_only, r.state.quantity());
    if (const std::string_view denied = risk.check_modify(check); !denied.empty()) {
      model::OrderModifyRejected rejected;
      rejected.header = header(now, s, r);
      static_cast<void>(model::ReasonText::from(denied, rejected.reason));
      rejected.venue_order_id = r.venue_order_id;
      rejected.account_id = account_id;
      queue(s, KernelEvent{model::OrderEvent{rejected}});
      return core::Status::Ok;
    }
    static_cast<void>(oms.apply(index, execution::OrderEventKind::PendingUpdate));
    static_cast<void>(outputs.emplace_back(
        std::in_place_type<model::ModifyOrder>,
        model::ModifyOrder{s, r.client_order_id, r.instrument_id, r.venue_order_id, q, p, now.ts}));
    model::OrderPendingUpdate event;
    event.header = header(now, s, r);
    event.account_id = account_id;
    event.venue_order_id = r.venue_order_id;
    static_cast<void>(events.push_back(PendingEvent{s, KernelEvent{model::OrderEvent{event}}}));
    ++stats.modifies;
    return core::Status::Ok;
  }

  // NotFound for an order this strategy does not own; InvalidState when it is closed or a
  // cancel is already pending.
  [[nodiscard]] core::Status cancel(const core::EventKey& now, StrategyIndex s,
                                    const model::ClientOrderId& cid, Outputs& outputs) noexcept {
    const std::uint32_t index = owned(s, cid);
    if (index != execution::kNoIndex) {
      return cancel_index(now, s, index, outputs);
    }
    const std::uint32_t parent = algos.find(s, cid);
    if (parent == execution::kNoIndex) {
      return core::Status::NotFound;
    }
    return cancel_parent(now, parent, outputs);
  }

  // A child order, canceled by its algorithm.
  [[nodiscard]] core::Status cancel_child(const core::EventKey& now, StrategyIndex s,
                                          const model::ClientOrderId& cid,
                                          Outputs& outputs) noexcept {
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
                                        std::uint32_t& canceled) {
    canceled = 0;
    // Parents first, so that no algorithm replaces a child canceled below.
    for (std::uint32_t i = 0; i < algos.size(); ++i) {
      const AlgoState& p = algos.at(i);
      if (!p.active || p.canceling || p.strategy != s ||
          (id != nullptr && !(p.intent.instrument_id == *id))) {
        continue;
      }
      const core::Status st = cancel_parent(now, i, outputs);
      if (!core::ok(st)) {
        return st;
      }
    }
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
  // and the strategy should receive it. An applied fill (or fill void) is booked in the
  // portfolio first, so the strategy sees its new position during on_order_event; the position
  // events it causes are queued behind it.
  // A child order's event then reaches its execution algorithm (which may send commands).
  [[nodiscard]] bool on_venue_event(const core::EventKey& now, const model::OrderEvent& event,
                                    StrategyIndex& owner, Outputs& outputs, core::Status& status) {
    status = core::Status::Ok;
    std::uint32_t index = execution::kNoIndex;
    switch (execution::apply_order_event(oms, event, index)) {
    case execution::EventOutcome::Applied:
      ++stats.venue_events;
      owner = oms.at(index).strategy;
      book(now, oms.at(index), event);
      if (oms.at(index).parent != execution::kNoIndex) {
        status = on_child_event(now, index, event, outputs);
      }
      return true;
    case execution::EventOutcome::UnknownOrder:
      ++stats.unknown_order_events;
      return false;
    case execution::EventOutcome::DuplicateFill:
      ++stats.duplicate_fills;
      status = book_late_commission(index, event);
      return false;
    case execution::EventOutcome::Refused:
      ++stats.refused_order_events;
      return false;
    case execution::EventOutcome::Stale:
      ++stats.stale_order_events;
      return false;
    }
    return false;
  }

  // A child's applied event: the parent's fills and working quantity, then the algorithm.
  [[nodiscard]] core::Status on_child_event(const core::EventKey& now, std::uint32_t index,
                                            const model::OrderEvent& event, Outputs& outputs) {
    const execution::OrderRecord& r = oms.at(index);
    const std::uint32_t parent = r.parent;
    if (parent >= algos.size() || !algos.at(parent).active ||
        algos.at(parent).parent_seq != r.parent_seq) {
      return core::Status::Ok; // the parent closed before this late event
    }
    std::uint64_t filled = 0;
    std::uint64_t voided = 0;
    if (const auto* f = std::get_if<model::OrderFilled>(&event)) {
      filled = f->last_qty.raw();
    } else if (const auto* v = std::get_if<model::OrderFillVoided>(&event)) {
      voided = v->voided_qty.raw();
    }
    const bool open = execution::is_open(r.state.status());
    algos.on_child(parent, r.client_order_id, filled, voided, r.state.leaves().raw(), open);
    AlgoState& p = algos.at(parent);
    AlgoContext ctx{*this, now, outputs, parent};
    const core::Status st =
        dispatch(p.kind, [&](const auto& algo) { return algo.on_event(p, ctx, AlgoEvent{event}); });
    static_cast<void>(algos.settle(parent));
    return st;
  }

  // The top of `slot`'s book moved: kept for AlgoContext::top before strategies hear of the
  // update (a parent they submit then starts from it), and told to the algorithms after
  // (on_top).
  void set_top(std::uint32_t slot, AlgoTop top) noexcept {
    if (slot < tops.size()) {
      tops[slot] = top;
    }
  }

  // Tells the active parents on `slot` whose algorithm hears quotes that the top moved.
  [[nodiscard]] core::Status on_top(const core::EventKey& now, std::uint32_t slot,
                                    Outputs& outputs) {
    if (slot >= tops.size()) {
      return core::Status::Ok;
    }
    const std::optional<AlgoTop>& stored = tops[slot];
    if (!stored) {
      return core::Status::Ok;
    }
    const AlgoTop top = *stored;
    if (algos.quoting(slot) == 0) {
      return core::Status::Ok;
    }
    for (std::uint32_t i = 0; i < algos.size(); ++i) {
      AlgoState& p = algos.at(i);
      if (!p.active || p.slot != slot || !wants_quotes(p.kind)) {
        continue;
      }
      AlgoContext ctx{*this, now, outputs, i};
      const core::Status st = dispatch(p.kind, [&](const auto& algo) {
        return algo.on_event(p, ctx, AlgoEvent{AlgoQuote{top}});
      });
      static_cast<void>(algos.settle(i));
      if (!core::ok(st)) {
        return st;
      }
    }
    return core::Status::Ok;
  }

  // The algorithms' timers due at `deadline`, earliest parent first by slot.
  [[nodiscard]] core::Status on_algo_timer(const core::EventKey& now, core::UnixNanos deadline,
                                           Outputs& outputs) {
    for (std::uint32_t i = 0; i < algos.size(); ++i) {
      AlgoState& p = algos.at(i);
      if (!p.active || p.wake_ns == 0 || p.wake_ns > deadline.value()) {
        continue;
      }
      p.wake_ns = 0;
      algo_wake_changed = true;
      AlgoContext ctx{*this, now, outputs, i};
      const core::Status st = dispatch(p.kind, [&](const auto& algo) {
        return algo.on_event(p, ctx, AlgoEvent{AlgoTimer{deadline}});
      });
      static_cast<void>(algos.settle(i));
      if (!core::ok(st)) {
        return st;
      }
    }
    return core::Status::Ok;
  }

  // The earliest algorithm timer, if any.
  [[nodiscard]] std::optional<core::UnixNanos> next_algo_wake() const noexcept {
    std::uint64_t next = 0;
    for (std::uint32_t i = 0; i < algos.size(); ++i) {
      const AlgoState& p = algos.at(i);
      if (p.active && p.wake_ns != 0 && (next == 0 || p.wake_ns < next)) {
        next = p.wake_ns;
      }
    }
    return next == 0 ? std::nullopt : std::optional{core::UnixNanos{next}};
  }

  // The header of an event the kernel synthesizes for order `index` at venue time `ts_event`
  // (reconciliation).
  [[nodiscard]] model::OrderEventHeader venue_header(const core::EventKey& now, std::uint32_t index,
                                                     core::UnixNanos ts_event) noexcept {
    const execution::OrderRecord& r = oms.at(index);
    model::OrderEventHeader h = header(now, r.strategy, r);
    h.ts_event = ts_event;
    return h;
  }

  // ---- market inputs the portfolio values positions with ----------------------------------

  // A funding rate update; a settlement queues a PositionAdjusted for every strategy holding the
  // instrument.
  [[nodiscard]] core::Status on_funding(const core::EventKey& now, std::uint32_t slot,
                                        const model::FundingRateUpdate& update) {
    const model::Instrument* def = definition(slot);
    if (def == nullptr) {
      return core::Status::Ok;
    }
    std::span<const portfolio::FundingShare> shares;
    const core::Status s = portfolio.on_funding(*def, slot, update, shares);
    if (!core::ok(s)) {
      return s;
    }
    for (const portfolio::FundingShare& share : shares) {
      model::PositionAdjusted adjusted;
      adjusted.header =
          position_header(now, share.strategy, model::common(*def).id, update.ts_event);
      adjusted.adjustment_type = model::PositionAdjustmentType::Funding;
      adjusted.pnl_change = share.payment;
      model::PositionEvent pe{adjusted};
      queue(share.strategy, KernelEvent{pe});
    }
    return core::Status::Ok;
  }

  // ---- risk: TradingState, monitors, KillSwitch ---------------------------------------------

  // Runs the post-trade monitors on the account's equity; a hard limit halts trading and fires
  // the KillSwitch (every open order of every strategy is canceled).
  [[nodiscard]] core::Status monitor(const core::EventKey& now, Outputs& outputs) {
    const std::optional<model::Currency> currency = monitor_currency();
    std::int64_t wallet = 0;
    if (!currency || !portfolio.wallet(*currency, wallet)) {
      return core::Status::Ok;
    }
    std::int64_t unrealized = 0;
    std::int64_t initial = 0;
    std::int64_t maintenance = 0;
    exposure_totals(*currency, unrealized, initial, maintenance);
    const std::optional<risk::TradingTrigger> trigger =
        risk.monitor().observe(now.ts, wallet + unrealized, maintenance);
    if (!trigger) {
      return core::Status::Ok;
    }
    const bool changed = risk.apply(*trigger);
    if (changed && *trigger == risk::TradingTrigger::HardLimit) {
      return kill_switch(now, outputs);
    }
    return core::Status::Ok;
  }

  // Cancels every cancelable order of every strategy.
  [[nodiscard]] core::Status kill_switch(const core::EventKey& now, Outputs& outputs) {
    risk.note_kill_switch();
    for (std::uint32_t s = 0; s < strategy_ids.size(); ++s) {
      std::uint32_t canceled = 0;
      const core::Status st =
          cancel_all(now, static_cast<StrategyIndex>(s), nullptr, outputs, canceled);
      if (!core::ok(st)) {
        return st;
      }
    }
    return core::Status::Ok;
  }

  // Node lifecycle -> TradingState holds (section 10.2).
  void on_lifecycle(model::NodeState from, model::NodeState to) noexcept {
    if (to == model::NodeState::Syncing) {
      if (from == model::NodeState::Degraded) {
        risk.apply(risk::TradingTrigger::Recovered);
      }
      risk.apply(risk::TradingTrigger::SyncStarted);
    } else if (to == model::NodeState::Running) {
      risk.apply(risk::TradingTrigger::Synced);
    } else if (to == model::NodeState::Degraded) {
      risk.apply(risk::TradingTrigger::Degraded);
    }
  }

  // Available margin in `currency` (10^9 raw): wallet + unrealized PnL - initial margin of the
  // venue positions and of the open orders settled in it. False when the account never held it.
  [[nodiscard]] bool available(const model::Currency& currency, std::int64_t& out) const {
    std::int64_t wallet = 0;
    if (!portfolio.wallet(currency, wallet)) {
      return false;
    }
    std::int64_t unrealized = 0;
    std::int64_t initial = 0;
    std::int64_t maintenance = 0;
    exposure_totals(currency, unrealized, initial, maintenance);
    out = wallet + unrealized - initial;
    return true;
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

  // The strategy's position in instrument `slot`; false without an instrument definition.
  [[nodiscard]] bool position(StrategyIndex s, std::uint32_t slot, PositionView& out) const {
    const model::Instrument* def = definition(slot);
    if (def == nullptr || s >= portfolio.strategies()) {
      return false;
    }
    const model::InstrumentCommon& c = model::common(*def);
    const portfolio::NettingPosition& p = portfolio.position(s, slot);
    out = PositionView{};
    out.instrument_id = c.id;
    out.position_id = position_id(s, c.id);
    out.side = p.side();
    static_cast<void>(model::Decimal::from_raw(p.signed_raw(), c.size_precision, out.signed_qty));
    static_cast<void>(model::Quantity::from_raw(p.quantity_raw(), c.size_precision, out.quantity));
    model::Price avg;
    if (p.avg_px_open(avg)) {
      out.avg_px_open = avg;
    }
    const model::Currency& ccy = c.settlement_currency;
    std::int64_t unrealized = 0;
    static_cast<void>(portfolio.unrealized(c, slot, p, unrealized));
    money(p.realized_raw() - p.commission_raw() + p.funding_raw(), ccy, out.realized_pnl);
    money(unrealized, ccy, out.unrealized_pnl);
    money(p.commission_raw(), ccy, out.commission);
    money(p.funding_raw(), ccy, out.funding);
    money(p.total_realized_raw() - p.total_commission_raw() + p.total_funding_raw(), ccy,
          out.total_pnl);
    out.ts_opened = p.ts_opened();
    return true;
  }

  // open_exposure() of instrument `slot`; false without an instrument definition.
  [[nodiscard]] bool exposure(std::uint32_t slot, ExposureView& out) const {
    const model::Instrument* def = definition(slot);
    if (def == nullptr) {
      return false;
    }
    const model::InstrumentCommon& c = model::common(*def);
    execution::OpenQuantity open = oms.open_quantity(slot);
    open.buy_raw += algos.reserved(slot, model::OrderSide::Buy);
    open.sell_raw += algos.reserved(slot, model::OrderSide::Sell);
    const std::int64_t position = portfolio.venue(slot).signed_raw();
    out = ExposureView{};
    out.instrument_id = c.id;
    const auto clamp = [](core::i128 v) {
      if (v > INT64_MAX) {
        return INT64_MAX;
      }
      return v < INT64_MIN ? INT64_MIN : static_cast<std::int64_t>(v);
    };
    const std::int64_t max_long = clamp(core::i128{position} + open.buy_raw);
    const std::int64_t max_short = clamp(core::i128{position} - open.sell_raw);
    static_cast<void>(model::Decimal::from_raw(position, c.size_precision, out.position));
    static_cast<void>(model::Quantity::from_raw(open.buy_raw, c.size_precision, out.open_buy));
    static_cast<void>(model::Quantity::from_raw(open.sell_raw, c.size_precision, out.open_sell));
    static_cast<void>(model::Decimal::from_raw(max_long, c.size_precision, out.max_long));
    static_cast<void>(model::Decimal::from_raw(max_short, c.size_precision, out.max_short));
    const std::uint64_t worst = core::magnitude(max_long) > core::magnitude(max_short)
                                    ? core::magnitude(max_long)
                                    : core::magnitude(max_short);
    const std::optional<model::Price> price = portfolio.valuation(slot);
    model::Quantity q;
    model::Money notional;
    if (price && core::ok(model::Quantity::from_raw(worst, c.size_precision, q)) &&
        core::ok(model::notional_value(c, q, *price, notional))) {
      out.notional = notional;
    }
    return true;
  }

  // Balance of `currency`: total is the wallet balance, locked the initial margin of the venue
  // positions settled in it, free the rest (negative when the margin exceeds the wallet).
  [[nodiscard]] bool balance(const model::Currency& currency, model::AccountBalance& out) const {
    std::int64_t wallet = 0;
    if (!portfolio.wallet(currency, wallet)) {
      return false;
    }
    std::int64_t locked = 0;
    for (std::uint32_t slot = 0; slot < definitions.size(); ++slot) {
      const model::Instrument* def = definition(slot);
      if (def == nullptr || !portfolio.venue(slot).is_open() ||
          !(model::common(*def).settlement_currency == currency)) {
        continue;
      }
      model::Money initial;
      model::Money maintenance;
      if (core::ok(portfolio.margins(model::common(*def), slot, initial, maintenance))) {
        locked += initial.raw();
      }
    }
    model::Money total;
    model::Money held;
    model::Money free;
    money(wallet, currency, total);
    money(locked, currency, held);
    money(total.raw() - held.raw(), currency, free);
    return core::ok(model::AccountBalance::create(total, held, free, out));
  }

  // ---- state --------------------------------------------------------------------------------

  execution::Oms oms;
  core::FixedVector<std::optional<model::Instrument>> definitions; // by instrument slot
  model::ClientOrderIdGenerator ids;
  model::TraderId trader_id;
  model::AccountId account_id;
  core::FixedVector<model::StrategyId> strategy_ids;
  core::FixedVector<PendingEvent> events; // kernel-produced, delivered after the callback
  portfolio::Portfolio portfolio;
  risk::RiskEngine risk;
  AlgoBook algos;
  execution::Reconciler reconciler;
  core::FixedVector<std::optional<AlgoTop>> tops; // by instrument slot, for the algorithms
  bool algo_wake_changed = false;                 // the engine re-arms its algorithm timer
  TradingStats stats;

  // Snapshot encoding (core/state.hpp). The trader and account ids come from the configuration;
  // the strategy ids are checked (a snapshot restores into the same strategies).
  template <typename Ar> void state(Ar& ar) {
    ar(oms, definitions, ids, strategy_ids, events, portfolio, risk, algos, reconciler, tops,
       algo_wake_changed, stats);
  }

private:
  // Calls f with the built-in algorithm of `kind`.
  template <typename F> static core::Status dispatch(AlgoKind kind, F&& f) {
    switch (kind) {
    case AlgoKind::Passthrough:
      return std::forward<F>(f)(Passthrough{});
    case AlgoKind::PassiveThenAggressive:
      return std::forward<F>(f)(PassiveThenAggressive{});
    case AlgoKind::PeggedQuote:
      return std::forward<F>(f)(PeggedQuote{});
    }
    return core::Status::InvalidArgument;
  }

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
    static_cast<void>(events.push_back(PendingEvent{s, KernelEvent{model::OrderEvent{event}}}));
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
    static_cast<void>(events.push_back(PendingEvent{s, KernelEvent{model::OrderEvent{denied}}}));
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

  // What the gates see of an order. `replaced` is the quantity a modify replaces.
  [[nodiscard]] risk::OrderCheck order_check(const core::EventKey& now, StrategyIndex s,
                                             std::uint32_t slot, const model::InstrumentCommon& c,
                                             model::OrderSide side, model::OrderType type,
                                             model::Quantity quantity,
                                             std::optional<model::Price> price, bool reduce_only,
                                             std::optional<model::Quantity> replaced) const {
    risk::OrderCheck check;
    check.instrument = &c;
    check.slot = slot;
    check.strategy = s;
    check.side = side;
    check.type = type;
    check.quantity = quantity;
    check.price = price;
    check.reduce_only = reduce_only;
    check.now = now.ts;
    check.position_raw = portfolio.venue(slot).signed_raw();
    check.open = oms.open_quantity(slot);
    check.open.buy_raw += algos.reserved(slot, model::OrderSide::Buy);
    check.open.sell_raw += algos.reserved(slot, model::OrderSide::Sell);
    check.reference = portfolio.valuation(slot);
    if (replaced) {
      check.kind = quantity.raw() > replaced->raw() ? risk::CommandKind::ModifyUp
                                                    : risk::CommandKind::Modify;
      return check;
    }
    const bool opposite =
        check.position_raw != 0 && (check.position_raw > 0) != (side == model::OrderSide::Buy);
    const bool reduces = opposite && quantity.raw() <= core::magnitude(check.position_raw);
    check.kind = reduces ? risk::CommandKind::Reduce : risk::CommandKind::Open;
    const std::optional<model::Price> at = price ? price : check.reference;
    model::Money required;
    if (risk.config().check_margin && !reduces && at &&
        available(c.settlement_currency, check.available_raw) &&
        core::ok(portfolio.order_margin(c, quantity, *at, required))) {
      check.margin_known = true;
      check.required_raw = required.raw();
    }
    return check;
  }

  // Unrealized PnL and initial and maintenance margin of every venue position and open order
  // settled in `currency` (10^9 raw).
  void exposure_totals(const model::Currency& currency, std::int64_t& unrealized,
                       std::int64_t& initial, std::int64_t& maintenance) const {
    unrealized = 0;
    initial = 0;
    maintenance = 0;
    for (std::uint32_t slot = 0; slot < definitions.size(); ++slot) {
      const model::Instrument* def = definition(slot);
      if (def == nullptr) {
        continue;
      }
      const model::InstrumentCommon& c = model::common(*def);
      if (!(c.settlement_currency == currency)) {
        continue;
      }
      const portfolio::NettingPosition& p = portfolio.venue(slot);
      if (p.is_open()) {
        std::int64_t u = 0;
        model::Money im;
        model::Money mm;
        if (core::ok(portfolio.unrealized(c, slot, p, u))) {
          unrealized += u;
        }
        if (core::ok(portfolio.margins(c, slot, im, mm))) {
          initial += im.raw();
          maintenance += mm.raw();
        }
      }
      const execution::OpenQuantity open = oms.open_quantity(slot);
      const core::u128 notional = open.buy_notional + open.sell_notional; // 10^18 per unit
      if (notional != 0) {
        const std::uint64_t rate =
            std::visit([&c](const auto& m) { return m.initial_rate(c); }, portfolio.margin());
        // notional x multiplier / 10^18 = 10^9 raw; x rate / 10^9, rounded up.
        const core::U192 scaled = core::div_u192_u64(
            core::div_u192_u64(core::mul_u128_u64(notional, c.multiplier.raw()), 1'000'000'000ULL),
            1'000'000'000ULL);
        std::uint64_t margin = 0;
        if (scaled.hi == 0 && scaled.mid == 0 &&
            core::ok(core::mul_div_u64_up(scaled.lo, rate, 1, 1'000'000'000ULL, margin)) &&
            margin <= static_cast<std::uint64_t>(INT64_MAX)) {
          initial += static_cast<std::int64_t>(margin);
        }
      }
    }
  }

  // The currency the loss monitors measure equity in: that of the first configured loss limit,
  // else the settlement currency of the first defined instrument.
  [[nodiscard]] std::optional<model::Currency> monitor_currency() const {
    const risk::RiskConfig& c = risk.config();
    for (const std::optional<model::Money>* limit :
         {&c.daily_loss_limit, &c.daily_loss_halt, &c.max_drawdown}) {
      if (*limit) {
        return (*limit)->currency();
      }
    }
    for (std::uint32_t slot = 0; slot < definitions.size(); ++slot) {
      if (const model::Instrument* def = definition(slot)) {
        return model::common(*def).settlement_currency;
      }
    }
    return std::nullopt;
  }

  void queue(StrategyIndex s, const KernelEvent& e) noexcept {
    if (!core::ok(events.push_back(PendingEvent{s, e}))) {
      ++stats.dropped_events;
    }
  }

  static void money(std::int64_t raw, const model::Currency& currency, model::Money& out) noexcept {
    if (!core::ok(model::Money::from_raw_truncated(raw, currency, out))) {
      static_cast<void>(model::Money::from_raw(0, currency, out));
    }
  }

  // "{instrument_id}-{strategy_id}" (nautilus NETTING position id).
  [[nodiscard]] model::PositionId position_id(StrategyIndex s,
                                              const model::InstrumentId& id) const noexcept {
    std::array<char, model::PositionIdRule::kCapacity> text{};
    std::size_t n = 0;
    const auto append = [&](std::string_view part) {
      for (const char ch : part) {
        if (n < text.size()) {
          text[n++] = ch;
        }
      }
    };
    const auto iid = id.text();
    append(iid.view());
    append("-");
    if (s < strategy_ids.size()) {
      append(strategy_ids[s].view());
    }
    model::PositionId out;
    static_cast<void>(model::PositionId::from(std::string_view{text.data(), n}, out));
    return out;
  }

  [[nodiscard]] model::PositionEventHeader position_header(const core::EventKey& now,
                                                           StrategyIndex s,
                                                           const model::InstrumentId& id,
                                                           core::UnixNanos ts_event) noexcept {
    model::PositionEventHeader h;
    h.trader_id = trader_id;
    if (s < strategy_ids.size()) {
      h.strategy_id = strategy_ids[s];
    }
    h.instrument_id = id;
    h.position_id = position_id(s, id);
    h.account_id = account_id;
    h.event_id = model::Uuid4::derive(event_rng_, now.seq, event_serial_++);
    h.ts_event = ts_event;
    h.ts_init = now.ts;
    return h;
  }

  // Books an applied fill or fill void and queues the strategy's position events.
  // The second report of a Lite fill brings its commission (docs/architecture.md section 8.3):
  // booked once, whatever further duplicates arrive.
  [[nodiscard]] core::Status book_late_commission(std::uint32_t index,
                                                  const model::OrderEvent& event) {
    const auto* f = std::get_if<model::OrderFilled>(&event);
    const bool lite =
        f != nullptr && (f->info_flags & static_cast<std::uint8_t>(model::FillInfo::Lite)) != 0;
    if (f == nullptr || lite || !f->commission ||
        !oms.take_pending_commission(index, f->trade_id)) {
      return core::Status::Ok;
    }
    const execution::OrderRecord& r = oms.at(index);
    const model::Instrument* def = definition(r.slot);
    return def == nullptr ? core::Status::Ok
                          : portfolio.on_commission(*def, r.slot, r.strategy, *f->commission);
  }

  void book(const core::EventKey& now, const execution::OrderRecord& r,
            const model::OrderEvent& event) {
    const model::Instrument* def = definition(r.slot);
    if (def == nullptr) {
      return;
    }
    model::OrderSide side = r.side;
    model::Quantity qty;
    model::Price px;
    std::optional<model::Money> commission;
    core::UnixNanos ts_event;
    if (const auto* f = std::get_if<model::OrderFilled>(&event)) {
      qty = f->last_qty;
      px = f->last_px;
      commission = f->commission;
      ts_event = f->header.ts_event;
    } else if (const auto* v = std::get_if<model::OrderFillVoided>(&event)) {
      // A void reverses the fill: the opposite side at the fill's price, commission refunded.
      side = r.side == model::OrderSide::Buy ? model::OrderSide::Sell : model::OrderSide::Buy;
      qty = v->voided_qty;
      px = v->last_px;
      if (v->commission_voided) {
        model::Money refund;
        if (core::ok(model::Money::from_raw(-v->commission_voided->raw(),
                                            v->commission_voided->currency(), refund))) {
          commission = refund;
        }
      }
      ts_event = v->header.ts_event;
    } else {
      return;
    }
    portfolio::FillOutcome outcome;
    if (!core::ok(portfolio.on_fill(*def, r.slot, r.strategy, side, qty, px, commission,
                                    r.client_order_id, ts_event, outcome)) ||
        !outcome.booked) {
      return;
    }
    const model::InstrumentCommon& c = model::common(*def);
    for (std::size_t i = 0; i < outcome.count; ++i) {
      queue(r.strategy, KernelEvent{position_event(now, r, c, outcome.parts[i], outcome.after[i],
                                                   px, ts_event)});
    }
  }

  [[nodiscard]] model::PositionEvent
  position_event(const core::EventKey& now, const execution::OrderRecord& r,
                 const model::InstrumentCommon& c, const portfolio::FillPart& part,
                 const portfolio::NettingPosition& p, model::Price px, core::UnixNanos ts_event) {
    const model::Currency& ccy = c.settlement_currency;
    const model::PositionEventHeader header = position_header(now, r.strategy, c.id, ts_event);
    model::Decimal signed_qty;
    model::Quantity quantity;
    model::Quantity peak;
    static_cast<void>(model::Decimal::from_raw(p.signed_raw(), c.size_precision, signed_qty));
    static_cast<void>(model::Quantity::from_raw(p.quantity_raw(), c.size_precision, quantity));
    static_cast<void>(model::Quantity::from_raw(p.peak_raw(), c.size_precision, peak));
    model::Price avg_open;
    static_cast<void>(p.avg_px_open(avg_open));
    model::Money realized;
    money(p.realized_raw() - p.commission_raw() + p.funding_raw(), ccy, realized);
    if (part.step == portfolio::PositionStep::Opened) {
      model::PositionOpened e;
      e.header = header;
      e.opening_order_id = p.opening_order_id();
      e.entry = p.entry();
      e.side = p.side();
      e.signed_qty = signed_qty;
      e.quantity = quantity;
      e.last_qty = part.quantity;
      e.last_px = px;
      e.currency = ccy;
      e.avg_px_open = avg_open;
      e.realized_pnl = realized;
      return model::PositionEvent{e};
    }
    std::optional<model::Price> avg_close;
    model::Price close;
    if (p.avg_px_close(close)) {
      avg_close = close;
    }
    model::Decimal realized_return;
    if (avg_close && avg_open.raw() > 0) {
      const core::i128 diff = core::i128{close.raw()} - avg_open.raw();
      const core::i128 ret = diff * 1'000'000'000 / avg_open.raw();
      const bool long_entry = p.entry() == model::OrderSide::Buy;
      static_cast<void>(model::Decimal::from_raw(static_cast<std::int64_t>(long_entry ? ret : -ret),
                                                 model::kFixedPrecision, realized_return));
    }
    std::int64_t unrealized_raw = 0;
    static_cast<void>(p.unrealized(px, c.multiplier.raw(), unrealized_raw));
    model::Money unrealized;
    money(unrealized_raw, ccy, unrealized);
    if (part.step == portfolio::PositionStep::Changed) {
      model::PositionChanged e;
      e.header = header;
      e.opening_order_id = p.opening_order_id();
      e.entry = p.entry();
      e.side = p.side();
      e.signed_qty = signed_qty;
      e.quantity = quantity;
      e.peak_quantity = peak;
      e.last_qty = part.quantity;
      e.last_px = px;
      e.currency = ccy;
      e.avg_px_open = avg_open;
      e.avg_px_close = avg_close;
      e.realized_return = realized_return;
      e.realized_pnl = realized;
      e.unrealized_pnl = unrealized;
      e.ts_opened = p.ts_opened();
      return model::PositionEvent{e};
    }
    model::PositionClosed e;
    e.header = header;
    e.opening_order_id = p.opening_order_id();
    e.closing_order_id = r.client_order_id;
    e.entry = p.entry();
    e.side = model::PositionSide::Flat;
    e.signed_qty = signed_qty;
    e.quantity = quantity;
    e.peak_quantity = peak;
    e.last_qty = part.quantity;
    e.last_px = px;
    e.currency = ccy;
    e.avg_px_open = avg_open;
    e.avg_px_close = avg_close;
    e.realized_return = realized_return;
    e.realized_pnl = realized;
    e.unrealized_pnl = unrealized;
    e.duration = core::DurationNanos{
        ts_event.value() >= p.ts_opened().value() ? ts_event.value() - p.ts_opened().value() : 0};
    e.ts_opened = p.ts_opened();
    e.ts_closed = ts_event;
    return model::PositionEvent{e};
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
    model::ClientOrderId parent_id;
    if (r.parent != execution::kNoIndex && core::ok(ids.id_of(r.parent_seq, parent_id))) {
      v.parent_id = parent_id;
    }
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

static_assert(ExecAlgorithm<Passthrough, Trading::AlgoContext>);

} // namespace jarvis::strategy
