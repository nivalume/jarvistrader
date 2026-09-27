#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
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
  Passthrough = 0, // the parent goes out as one child on the same terms
};
inline constexpr std::size_t kAlgoKindCount = 1;

[[nodiscard]] constexpr std::string_view to_string(AlgoKind k) noexcept {
  switch (k) {
  case AlgoKind::Passthrough:
    return "passthrough";
  }
  return "";
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
};

struct AlgoChild {
  model::ClientOrderId id;
  std::uint64_t leaves_raw = 0;
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
  bool active = false;
  bool canceling = false;
  bool finished = false;

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
    refresh(i);
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

// What an algorithm hears about once its parent started: its children's order events (quotes
// and timers join with the quoting algorithms, plan.md M5).
using AlgoEvent = model::OrderEvent;

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

} // namespace jarvis::strategy
