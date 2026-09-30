#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_intent.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/order_events.hpp"

// Execution algorithms (docs/architecture.md section 11.4). A strategy submits a parent order
// with ctx.submit_parent; the parent passes Gate A as an intent, and an algorithm then works it
// through child orders, which pass Gate B like any order. Algorithms run inside step and keep
// their state in AlgoState (a fixed arena, AlgoBook), so they are as deterministic as the rest
// of the kernel.
//
// The algorithms are a closed set of built-ins chosen by AlgoKind; each satisfies ExecAlgorithm
// for the kernel's AlgoContext (strategy::Trading::AlgoContext), which is what they may do:
// submit, modify and cancel their children (Gate B and the rate limit apply), read the rate
// budget, and mark themselves finished.
//
// Parent orders never reach the venue and have no order events of their own; the strategy
// receives its children's events (OrderView::parent_id names the parent) and can read the
// parent with ctx.parent. A parent closes when it is filled, or when it was canceled or its
// algorithm finished and no child is still working.
//
// open_exposure() counts a parent's remaining quantity that no child is working yet (section
// 9.3), so ten slices of one parent cannot each pass the position limit on their own.

namespace jarvis::strategy {

using data::StrategyIndex;

enum class AlgoKind : std::uint8_t {
  Passthrough = 0,           // the parent goes out as one child on the same terms
  PassiveThenAggressive = 1, // rest passively, then take what is left
  PeggedQuote = 2,           // one resting order kept at an offset from a reference price
};
inline constexpr std::size_t kAlgoKindCount = 3;

[[nodiscard]] constexpr std::string_view to_string(AlgoKind k) noexcept {
  switch (k) {
  case AlgoKind::Passthrough:
    return "passthrough";
  case AlgoKind::PassiveThenAggressive:
    return "passive_then_aggressive";
  case AlgoKind::PeggedQuote:
    return "pegged_quote";
  }
  return "";
}

// Whether the algorithm hears of its instrument's quotes (AlgoQuote).
[[nodiscard]] constexpr bool wants_quotes(AlgoKind k) noexcept {
  return k == AlgoKind::PassiveThenAggressive || k == AlgoKind::PeggedQuote;
}

[[nodiscard]] constexpr bool parse_algo(std::string_view text, AlgoKind& out) noexcept {
  for (std::size_t i = 0; i < kAlgoKindCount; ++i) {
    const auto k = static_cast<AlgoKind>(i);
    if (to_string(k) == text) {
      out = k;
      return true;
    }
  }
  return false;
}

inline constexpr std::size_t kAlgoParams = 4;   // numeric parameters of one parent
inline constexpr std::size_t kAlgoScratch = 8;  // the algorithm's own state
inline constexpr std::size_t kAlgoChildren = 4; // children working at once

struct AlgoParams {
  std::array<std::int64_t, kAlgoParams> values{};

  template <typename Ar> void state(Ar& ar) { ar(values); }
};

struct AlgoChild {
  model::ClientOrderId id;
  std::uint64_t leaves_raw = 0;

  template <typename Ar> void state(Ar& ar) { ar(id, leaves_raw); }
};

// One parent order.
struct AlgoState {
  model::ClientOrderId parent_id;
  std::uint64_t parent_seq = 0; // the sequence number of parent_id (child orders keep it)
  execution::OrderIntent intent;
  AlgoKind kind = AlgoKind::Passthrough;
  AlgoParams params;
  StrategyIndex strategy = 0;
  std::uint32_t slot = 0;         // instrument slot
  std::uint64_t filled_raw = 0;   // filled by children (voids subtracted)
  std::uint64_t reserved_raw = 0; // counted in open_exposure: remaining that no child works
  std::array<AlgoChild, kAlgoChildren> children{};
  std::uint8_t child_count = 0;
  std::array<std::int64_t, kAlgoScratch> scratch{};
  std::uint64_t wake_ns = 0; // the algorithm's timer (AlgoTimer); 0: none
  bool active = false;
  bool canceling = false;
  bool finished = false;

  template <typename Ar> void state(Ar& ar) {
    ar(parent_id, parent_seq, intent, kind, params, strategy, slot, filled_raw, reserved_raw,
       children, child_count, scratch, wake_ns, active, canceling, finished);
  }

  [[nodiscard]] std::uint64_t remaining_raw() const noexcept {
    const std::uint64_t q = intent.quantity.raw();
    return filled_raw >= q ? 0 : q - filled_raw;
  }
  [[nodiscard]] std::uint64_t working_raw() const noexcept {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < child_count; ++i) {
      sum += children[i].leaves_raw;
    }
    return sum;
  }
};

// The parents of every strategy: a fixed arena plus the reserved quantity per instrument and
// side that open_exposure() adds.
class AlgoBook {
public:
  AlgoBook(std::uint32_t parents, std::uint32_t instruments)
      : parents_{parents}, reserved_{instruments} {
    for (std::uint32_t i = 0; i < parents; ++i) {
      static_cast<void>(parents_.push_back(AlgoState{}));
    }
    for (std::uint32_t i = 0; i < instruments; ++i) {
      static_cast<void>(reserved_.push_back(Reserve{}));
    }
  }

  // Active parents on `slot` whose algorithm hears quotes (the kernel skips the rest).
  [[nodiscard]] std::uint32_t quoting(std::uint32_t slot) const noexcept {
    return slot < reserved_.size() ? reserved_[slot].quoting : 0;
  }

  // Stores a new active parent in a free slot; CapacityExceeded when every slot is active.
  [[nodiscard]] core::Status open(const AlgoState& parent, std::uint32_t& index) noexcept {
    for (std::uint32_t i = 0; i < parents_.size(); ++i) {
      if (!parents_[i].active) {
        parents_[i] = parent;
        parents_[i].active = true;
        parents_[i].reserved_raw = 0;
        parents_[i].child_count = 0;
        index = i;
        refresh(i);
        if (wants_quotes(parent.kind) && parent.slot < reserved_.size()) {
          ++reserved_[parent.slot].quoting;
        }
        return core::Status::Ok;
      }
    }
    return core::Status::CapacityExceeded;
  }

  [[nodiscard]] AlgoState& at(std::uint32_t i) noexcept { return parents_[i]; }
  [[nodiscard]] const AlgoState& at(std::uint32_t i) const noexcept { return parents_[i]; }
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(parents_.size());
  }

  // The active parent `id` of strategy `s`, or kNoIndex.
  [[nodiscard]] std::uint32_t find(StrategyIndex s, const model::ClientOrderId& id) const noexcept {
    for (std::uint32_t i = 0; i < parents_.size(); ++i) {
      const AlgoState& p = parents_[i];
      if (p.active && p.strategy == s && p.parent_id == id) {
        return i;
      }
    }
    return execution::kNoIndex;
  }

  // A child now working `leaves_raw` of the parent; CapacityExceeded with kAlgoChildren working.
  [[nodiscard]] core::Status add_child(std::uint32_t i, const model::ClientOrderId& id,
                                       std::uint64_t leaves_raw) noexcept {
    AlgoState& p = parents_[i];
    if (p.child_count >= kAlgoChildren) {
      return core::Status::CapacityExceeded;
    }
    p.children[p.child_count++] = AlgoChild{id, leaves_raw};
    refresh(i);
    return core::Status::Ok;
  }

  // A child's order event was applied: `filled_raw` was filled (`voided_raw` voided), the child
  // now has `leaves_raw` left, and `open` says whether the venue may still fill it.
  void on_child(std::uint32_t i, const model::ClientOrderId& id, std::uint64_t filled_raw,
                std::uint64_t voided_raw, std::uint64_t leaves_raw, bool open) noexcept {
    AlgoState& p = parents_[i];
    p.filled_raw += filled_raw;
    p.filled_raw = voided_raw > p.filled_raw ? 0 : p.filled_raw - voided_raw;
    for (std::size_t k = 0; k < p.child_count; ++k) {
      if (!(p.children[k].id == id)) {
        continue;
      }
      if (open) {
        p.children[k].leaves_raw = leaves_raw;
      } else {
        p.children[k] = p.children[p.child_count - 1U];
        --p.child_count;
      }
      break;
    }
    refresh(i);
  }

  void cancel(std::uint32_t i) noexcept {
    parents_[i].canceling = true;
    refresh(i);
  }
  void finish(std::uint32_t i) noexcept {
    parents_[i].finished = true;
    refresh(i);
  }

  // Closes the parent when nothing more can happen: it is filled, or it was canceled or its
  // algorithm finished and no child is working. Returns whether it closed.
  bool settle(std::uint32_t i) noexcept {
    AlgoState& p = parents_[i];
    if (!p.active) {
      return false;
    }
    const bool done = p.remaining_raw() == 0 || p.canceling || p.finished;
    if (!done || p.child_count != 0) {
      return false;
    }
    p.active = false;
    p.wake_ns = 0;
    refresh(i);
    if (wants_quotes(p.kind) && p.slot < reserved_.size() && reserved_[p.slot].quoting > 0) {
      --reserved_[p.slot].quoting;
    }
    return true;
  }

  // The parents' quantity on `slot` and `side` that no child works yet.
  [[nodiscard]] std::uint64_t reserved(std::uint32_t slot, model::OrderSide side) const noexcept {
    if (slot >= reserved_.size()) {
      return 0;
    }
    return side == model::OrderSide::Buy ? reserved_[slot].buy_raw : reserved_[slot].sell_raw;
  }

private:
  struct Reserve {
    std::uint64_t buy_raw = 0;
    std::uint64_t sell_raw = 0;
    std::uint32_t quoting = 0; // active parents that hear quotes

    template <typename Ar> void state(Ar& ar) { ar(buy_raw, sell_raw, quoting); }
  };

  // Recomputes parent i's reserved quantity and moves the instrument totals by the difference.
  void refresh(std::uint32_t i) noexcept {
    AlgoState& p = parents_[i];
    std::uint64_t now = 0;
    if (p.active && !p.canceling && !p.finished) {
      const std::uint64_t remaining = p.remaining_raw();
      const std::uint64_t working = p.working_raw();
      now = working >= remaining ? 0 : remaining - working;
    }
    if (p.slot < reserved_.size()) {
      std::uint64_t& total = p.intent.side == model::OrderSide::Buy ? reserved_[p.slot].buy_raw
                                                                    : reserved_[p.slot].sell_raw;
      total = total - p.reserved_raw + now;
    }
    p.reserved_raw = now;
  }

  core::FixedVector<AlgoState> parents_;
  core::FixedVector<Reserve> reserved_; // by instrument slot

public:
  // Snapshot encoding (core/state.hpp).
  template <typename Ar> void state(Ar& ar) { ar(parents_, reserved_); }
};

// A parent order as strategies see it (ctx.parent).
struct ParentView {
  model::ClientOrderId parent_id;
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::Buy;
  AlgoKind kind = AlgoKind::Passthrough;
  model::Quantity quantity;
  model::Quantity filled;
  model::Quantity reserved; // remaining that no child works yet
  std::uint8_t children = 0;
  bool active = false;
  bool canceling = false;
};

// The best bid and ask of the parent's instrument and their displayed sizes (quotes, or the L2
// book's best levels).
struct AlgoTop {
  model::Price bid;
  model::Price ask;
  model::Quantity bid_size;
  model::Quantity ask_size;

  template <typename Ar> void state(Ar& ar) { ar(bid, ask, bid_size, ask_size); }
};

struct AlgoQuote { // the top moved (algorithms for which wants_quotes)
  AlgoTop top;
};
struct AlgoTimer { // the time the algorithm asked for with ctx.wake_at came
  core::UnixNanos deadline;
};

// What an algorithm hears about once its parent started.
using AlgoEvent = std::variant<model::OrderEvent, AlgoQuote, AlgoTimer>;

template <typename A, typename Ctx>
concept ExecAlgorithm = requires(const A a, AlgoState& st, Ctx& ctx, const AlgoEvent& e) {
  { a.on_parent(st, ctx) } -> std::same_as<core::Status>;   // the parent arrived
  { a.on_event(st, ctx, e) } -> std::same_as<core::Status>; // a child's order event
  { a.on_cancel(st, ctx) } -> std::same_as<core::Status>;   // the parent is being canceled
};

// The parent goes out as one child on the parent's terms. A denied or unfilled child ends it.
struct Passthrough {
  template <typename Ctx> [[nodiscard]] core::Status on_parent(AlgoState& st, Ctx& ctx) const {
    model::ClientOrderId child;
    bool denied = false;
    const core::Status s = ctx.submit(st.intent, child, denied);
    if (core::ok(s) && denied) {
      ctx.finish();
    }
    return s;
  }
  template <typename Ctx>
  [[nodiscard]] core::Status on_event(AlgoState& st, Ctx& ctx, const AlgoEvent& /*e*/) const {
    if (st.child_count == 0) {
      ctx.finish(); // the child closed (filled, canceled, expired or rejected): nothing to redo
    }
    return core::Status::Ok;
  }
  template <typename Ctx> [[nodiscard]] core::Status on_cancel(AlgoState& /*st*/, Ctx& ctx) const {
    return ctx.cancel_children();
  }
};

namespace algo_detail {

// `raw` rounded down to a multiple of `step` (a positive raw increment).
[[nodiscard]] constexpr std::int64_t floor_to(std::int64_t raw, std::int64_t step) noexcept {
  if (step <= 0) {
    return raw;
  }
  const std::int64_t r = raw % step;
  return r >= 0 ? raw - r : raw - r - step;
}

// A limit (or, without a price, market) child for `quantity_raw` on the parent's terms.
[[nodiscard]] inline execution::OrderIntent
child_of(const AlgoState& st, std::uint64_t quantity_raw, std::optional<model::Price> price,
         bool post_only, model::TimeInForce tif) noexcept {
  execution::OrderIntent c = st.intent;
  static_cast<void>(
      model::Quantity::from_raw(quantity_raw, st.intent.quantity.precision(), c.quantity));
  c.type = price ? model::OrderType::Limit : model::OrderType::Market;
  c.price = price;
  c.post_only = post_only;
  c.time_in_force = tif;
  return c;
}

[[nodiscard]] inline std::optional<model::Price> price_of(std::int64_t raw,
                                                          std::uint8_t precision) noexcept {
  model::Price p;
  if (raw <= 0 || !core::ok(model::Price::from_raw(raw, precision, p))) {
    return std::nullopt;
  }
  return p;
}

} // namespace algo_detail

// Rests the parent passively, then takes what is left (section 11.4).
//
//   params[0]  how long to rest, in nanoseconds (0: 5 s)
//   params[1]  how far the market may move away from the resting price, in ticks, before the
//              algorithm takes at once (0: no limit)
//   params[2]  the share of the parent's quantity that may be taken, per mille (0: all of it)
//
// The resting child is post-only at the parent's price when it has one, else at the same-side
// best. Taking cancels it and, once it is gone, sends an IOC limit at the opposite best (a
// market IOC without quotes) for what is left, within the share; after that the algorithm is
// done. A resting child the venue refuses (post-only that would cross) or that closes unfilled
// is taken at once too.
struct PassiveThenAggressive {
  static constexpr std::int64_t kDefaultRestNs = 5'000'000'000;
  enum Phase : std::uint8_t { kResting = 0, kSwitching = 1, kTaking = 2 };

  template <typename Ctx> [[nodiscard]] core::Status on_parent(AlgoState& st, Ctx& ctx) const {
    std::optional<model::Price> px =
        st.intent.type == model::OrderType::Limit ? st.intent.price : std::nullopt;
    if (!px) {
      if (const std::optional<AlgoTop> top = ctx.top()) {
        px = st.intent.side == model::OrderSide::Buy ? top->bid : top->ask;
      }
    }
    if (!px) {
      return take(st, ctx); // nothing to rest at
    }
    st.scratch[1] = px->raw();
    model::ClientOrderId child;
    bool denied = false;
    const core::Status s =
        ctx.submit(algo_detail::child_of(st, st.reserved_raw, px, true, model::TimeInForce::Gtc),
                   child, denied);
    if (!core::ok(s) || denied) {
      ctx.finish();
      return s;
    }
    const std::int64_t rest = st.params.values[0] > 0 ? st.params.values[0] : kDefaultRestNs;
    ctx.wake_at(core::UnixNanos{ctx.now().value() + static_cast<std::uint64_t>(rest)});
    return core::Status::Ok;
  }

  template <typename Ctx>
  [[nodiscard]] core::Status on_event(AlgoState& st, Ctx& ctx, const AlgoEvent& e) const {
    if (const auto* q = std::get_if<AlgoQuote>(&e)) {
      return st.scratch[0] == kResting && moved_away(st, ctx, q->top) ? switch_to_take(st, ctx)
                                                                      : core::Status::Ok;
    }
    if (std::holds_alternative<AlgoTimer>(e)) {
      return st.scratch[0] == kResting ? switch_to_take(st, ctx) : core::Status::Ok;
    }
    if (st.child_count != 0) {
      return core::Status::Ok;
    }
    if (st.scratch[0] == kTaking) {
      ctx.finish();
      return core::Status::Ok;
    }
    return take(st, ctx); // resting child gone (refused, canceled, expired) or switching done
  }

  template <typename Ctx> [[nodiscard]] core::Status on_cancel(AlgoState& /*st*/, Ctx& ctx) const {
    ctx.sleep();
    return ctx.cancel_children();
  }

private:
  template <typename Ctx>
  [[nodiscard]] static bool moved_away(const AlgoState& st, Ctx& ctx, const AlgoTop& top) {
    const std::optional<model::Price> tick = ctx.tick();
    if (st.params.values[1] <= 0 || !tick) {
      return false;
    }
    const std::int64_t limit = st.params.values[1] * tick->raw();
    return st.intent.side == model::OrderSide::Buy ? top.bid.raw() - st.scratch[1] > limit
                                                   : st.scratch[1] - top.ask.raw() > limit;
  }

  template <typename Ctx>
  [[nodiscard]] static core::Status switch_to_take(AlgoState& st, Ctx& ctx) {
    st.scratch[0] = kSwitching;
    ctx.sleep();
    return st.child_count == 0 ? take(st, ctx) : ctx.cancel_children();
  }

  template <typename Ctx> [[nodiscard]] static core::Status take(AlgoState& st, Ctx& ctx) {
    st.scratch[0] = kTaking;
    ctx.sleep();
    const std::uint64_t q = st.intent.quantity.raw();
    const std::int64_t share =
        st.params.values[2] > 0 && st.params.values[2] < 1000 ? st.params.values[2] : 1000;
    std::uint64_t allowed = static_cast<std::uint64_t>(
        (static_cast<core::u128>(q) * static_cast<std::uint64_t>(share)) / 1000U);
    if (const std::optional<model::Quantity> lot = ctx.lot()) {
      allowed = static_cast<std::uint64_t>(algo_detail::floor_to(
          static_cast<std::int64_t>(allowed), static_cast<std::int64_t>(lot->raw())));
    }
    const std::uint64_t taken = static_cast<std::uint64_t>(st.scratch[2]);
    std::uint64_t quantity = allowed > taken ? allowed - taken : 0;
    quantity = quantity < st.reserved_raw ? quantity : st.reserved_raw;
    if (quantity == 0) {
      ctx.finish();
      return core::Status::Ok;
    }
    std::optional<model::Price> px;
    if (const std::optional<AlgoTop> top = ctx.top()) {
      px = st.intent.side == model::OrderSide::Buy ? top->ask : top->bid;
    }
    model::ClientOrderId child;
    bool denied = false;
    const core::Status s = ctx.submit(
        algo_detail::child_of(st, quantity, px, false, model::TimeInForce::Ioc), child, denied);
    if (!core::ok(s) || denied) {
      ctx.finish();
      return s;
    }
    st.scratch[2] = static_cast<std::int64_t>(taken + quantity);
    return core::Status::Ok;
  }
};

// One resting order kept at an offset from a reference price (section 11.4), until the parent
// is filled or canceled.
//
//   params[0]  the offset, in ticks, away from the market (0: at the reference)
//   params[1]  the reference: 0 the same-side best, 1 the mid
//   params[2]  how far the target must move before the order follows, in ticks (0: 1)
//   params[3]  orders of rate budget to keep: the order follows only while more remain (0: 2)
//
// The order is post-only and never priced through the other side. On Binance USD-M a modify
// and a cancel-replace both put the order at the back of its new price's queue and both cost
// one order in the rate windows (a cancel costs none), so the order follows the reference by
// modifying its price, which leaves no gap and takes one round trip. What the queue decides is
// whether a move is worth following:
//
//   - queue estimate: when the order goes to a price, the quantity ahead of it is the displayed
//     size there if that is the best on its side, none if it improves the best, unknown if it
//     is behind. While its price is the best, a smaller displayed size shrinks what is ahead in
//     proportion (trades and cancels spread over the queue, as the QueuePosition fill model
//     assumes); a larger one joins behind it. Out of sight (behind the best) the estimate stays.
//   - an order at the front of its queue (less ahead of it than its own remaining quantity)
//     follows a move toward the market only one tick later than the threshold: a place at the
//     front is worth a tick. A move away from the market is followed at the threshold, since
//     the offset is what keeps the order out of the way.
//
// While a modify is pending, the order is not yet acknowledged, or the budget is short, it waits
// for the next quote. A modify the venue refuses is replaced: the order is canceled and a new one
// goes out at the first quote after the cancel is confirmed, as after the venue refuses or
// cancels an order itself.
struct PeggedQuote {
  // The algorithm's scratch slots.
  static constexpr std::size_t kPrice = 0;     // the order's price (raw)
  static constexpr std::size_t kAhead = 1;     // the estimated quantity ahead of it (raw)
  static constexpr std::size_t kLevel = 2;     // the displayed size at its price, while the best
  static constexpr std::size_t kReplacing = 3; // 1: canceled after a refused modify
  static constexpr std::int64_t kUnknown = -1;

  template <typename Ctx> [[nodiscard]] core::Status on_parent(AlgoState& st, Ctx& ctx) const {
    const std::optional<AlgoTop> top = ctx.top();
    return top ? quote(st, ctx, *top) : core::Status::Ok; // else at the first quote
  }

  template <typename Ctx>
  [[nodiscard]] core::Status on_event(AlgoState& st, Ctx& ctx, const AlgoEvent& e) const {
    if (const auto* q = std::get_if<AlgoQuote>(&e)) {
      return quote(st, ctx, q->top);
    }
    if (st.child_count == 0) {
      st.scratch[kReplacing] = 0;
      return core::Status::Ok; // the parent's fill accounting does the rest
    }
    const auto* event = std::get_if<model::OrderEvent>(&e);
    if (event != nullptr && std::holds_alternative<model::OrderModifyRejected>(*event) &&
        !st.canceling && st.scratch[kReplacing] == 0) {
      st.scratch[kReplacing] = 1;
      const core::Status s = ctx.cancel(st.children[0].id);
      return s == core::Status::InvalidState ? core::Status::Ok : s; // closing meanwhile
    }
    return core::Status::Ok;
  }

  template <typename Ctx> [[nodiscard]] core::Status on_cancel(AlgoState& /*st*/, Ctx& ctx) const {
    return ctx.cancel_children();
  }

private:
  [[nodiscard]] static bool buying(const AlgoState& st) noexcept {
    return st.intent.side == model::OrderSide::Buy;
  }
  [[nodiscard]] static std::int64_t best_of(const AlgoState& st, const AlgoTop& top) noexcept {
    return buying(st) ? top.bid.raw() : top.ask.raw();
  }
  [[nodiscard]] static std::int64_t size_of(const AlgoState& st, const AlgoTop& top) noexcept {
    return static_cast<std::int64_t>(buying(st) ? top.bid_size.raw() : top.ask_size.raw());
  }

  [[nodiscard]] static std::optional<model::Price> target(const AlgoState& st, model::Price tick,
                                                          const AlgoTop& top) {
    if (top.bid.raw() <= 0 || top.ask.raw() <= top.bid.raw()) {
      return std::nullopt;
    }
    const std::int64_t t = tick.raw();
    const bool buy = buying(st);
    std::int64_t ref = buy ? top.bid.raw() : top.ask.raw();
    if (st.params.values[1] == 1) {
      const std::int64_t mid = top.bid.raw() + (top.ask.raw() - top.bid.raw()) / 2;
      ref = buy ? algo_detail::floor_to(mid, t) : algo_detail::floor_to(mid + t - 1, t);
    }
    const std::int64_t offset = (st.params.values[0] > 0 ? st.params.values[0] : 0) * t;
    std::int64_t px = buy ? ref - offset : ref + offset;
    if (buy && px > top.ask.raw() - t) {
      px = top.ask.raw() - t; // post-only: never at or through the ask
    } else if (!buy && px < top.bid.raw() + t) {
      px = top.bid.raw() + t;
    }
    return algo_detail::price_of(px, tick.precision());
  }

  // The order went to `px`: it joins the back of that price's queue.
  static void placed(AlgoState& st, std::int64_t px, const AlgoTop& top) noexcept {
    const std::int64_t best = best_of(st, top);
    st.scratch[kPrice] = px;
    if (px == best) {
      st.scratch[kAhead] = size_of(st, top);
      st.scratch[kLevel] = size_of(st, top);
    } else if (buying(st) ? px > best : px < best) {
      st.scratch[kAhead] = 0; // it improves the best
      st.scratch[kLevel] = 0;
    } else {
      st.scratch[kAhead] = kUnknown; // behind the best: the queue there is out of sight
      st.scratch[kLevel] = 0;
    }
  }

  // The quantity ahead of the order follows the displayed size at its price.
  static void track(AlgoState& st, const AlgoTop& top) noexcept {
    const std::int64_t best = best_of(st, top);
    const std::int64_t size = size_of(st, top);
    const std::int64_t px = st.scratch[kPrice];
    std::int64_t& ahead = st.scratch[kAhead];
    std::int64_t& level = st.scratch[kLevel];
    if (px == best) {
      if (ahead == kUnknown) {
        ahead = size; // back in sight: at most all of it is ahead
      } else if (level == 0) {
        ahead = ahead < size ? ahead : size;
      } else if (size < level && size >= 0) {
        ahead = static_cast<std::int64_t>(
            (static_cast<core::u128>(ahead) * static_cast<std::uint64_t>(size)) /
            static_cast<std::uint64_t>(level));
      }
      level = size;
    } else if (buying(st) ? best < px : best > px) {
      ahead = 0; // nothing is left at its price but itself (or it is not in the book yet)
      level = 0;
    } else {
      level = 0; // behind the best again
    }
  }

  template <typename Ctx>
  [[nodiscard]] static core::Status quote(AlgoState& st, Ctx& ctx, const AlgoTop& top) {
    const std::optional<model::Price> tick = ctx.tick();
    if (st.canceling || st.finished || !tick) {
      return core::Status::Ok;
    }
    const std::optional<model::Price> px = target(st, *tick, top);
    if (!px) {
      return core::Status::Ok;
    }
    if (st.child_count == 0) {
      if (st.reserved_raw == 0) {
        return core::Status::Ok;
      }
      model::ClientOrderId child;
      bool denied = false;
      const core::Status s =
          ctx.submit(algo_detail::child_of(st, st.reserved_raw, px, true, model::TimeInForce::Gtc),
                     child, denied);
      if (core::ok(s) && !denied) {
        st.scratch[kReplacing] = 0;
        placed(st, px->raw(), top);
      }
      return s;
    }
    track(st, top);
    if (st.scratch[kReplacing] != 0) {
      return core::Status::Ok; // the cancel is on its way
    }
    const std::int64_t t = tick->raw();
    const std::int64_t move = px->raw() - st.scratch[kPrice];
    const bool toward = buying(st) ? move > 0 : move < 0;
    const std::int64_t ahead = st.scratch[kAhead];
    const bool front =
        ahead != kUnknown && ahead < static_cast<std::int64_t>(st.children[0].leaves_raw);
    const std::int64_t threshold =
        (st.params.values[2] > 0 ? st.params.values[2] : 1) * t + (toward && front ? t : 0);
    const std::int64_t reserve = st.params.values[3] > 0 ? st.params.values[3] : 2;
    const model::ClientOrderId& id = st.children[0].id;
    if ((move < 0 ? -move : move) < threshold || !ctx.acknowledged(id) || ctx.pending(id) ||
        static_cast<std::int64_t>(ctx.rate_budget()) <= reserve) {
      return core::Status::Ok;
    }
    const core::Status s = ctx.modify(id, std::nullopt, px);
    if (core::ok(s) && ctx.pending(id)) {
      placed(st, px->raw(), top); // else the risk gate refused it: the order stays where it is
    }
    return s == core::Status::InvalidState ? core::Status::Ok : s; // closing meanwhile
  }
};

} // namespace jarvis::strategy
