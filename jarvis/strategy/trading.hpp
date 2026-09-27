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
  std::uint32_t currencies = 16;     // balances the portfolio tracks
  portfolio::Margin margin = portfolio::StandardMargin{};
  risk::RiskConfig risk;
};

// An event the kernel produced for a strategy in this step.
using KernelEvent = std::variant<model::OrderEvent, model::PositionEvent>;

struct PendingEvent {
  StrategyIndex strategy = 0;
  KernelEvent event;
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
  std::uint64_t dropped_events = 0; // kernel events lost to a full queue (position events)
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
// leaves of open orders. Execution algorithms add their parents' remaining quantity (v1.x).
struct ExposureView {
  model::InstrumentId instrument_id;
  model::Decimal position;              // signed, raw at the instrument's size precision
  model::Quantity open_buy;             // leaves of open buy orders
  model::Quantity open_sell;            // leaves of open sell orders
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
        risk{c.risk, instruments, strategies}, event_rng_{seed ^ detail::kEventIdSalt} {
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
    if (reason.empty()) {
      const risk::OrderCheck check =
          order_check(now, s, slot, model::common(*def), intent.side, intent.type, intent.quantity,
                      intent.price, intent.reduce_only, std::nullopt);
      reason = risk.check_order(check);
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
  // and the strategy should receive it. An applied fill (or fill void) is booked in the
  // portfolio first, so the strategy sees its new position during on_order_event; the position
  // events it causes are queued behind it.
  [[nodiscard]] bool on_venue_event(const core::EventKey& now, const model::OrderEvent& event,
                                    StrategyIndex& owner) {
    std::uint32_t index = execution::kNoIndex;
    switch (execution::apply_order_event(oms, event, index)) {
    case execution::EventOutcome::Applied:
      ++stats.venue_events;
      owner = oms.at(index).strategy;
      book(now, oms.at(index), event);
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
    const execution::OpenQuantity open = oms.open_quantity(slot);
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
