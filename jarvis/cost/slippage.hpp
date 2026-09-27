#pragma once

#include <concepts>
#include <cstdint>
#include <span>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"

// Expected cost of taking liquidity (docs/architecture.md section 11.1). v1.0 has one static
// model: walk the opposite side of the book, best level first, until the quantity is filled.
// Levels are any type with `price` and `size` members (data::BookLevel from a BookView), best
// first, so the cost layer does not depend on the book implementation.

namespace jarvis::cost {

struct SlippageEstimate {
  model::Price best;         // the first level's price
  model::Price average;      // average fill price, rounded against the taker
  model::Quantity filled;    // at most the requested quantity
  bool complete = false;     // the levels held the whole quantity
  std::int64_t cost_raw = 0; // |average - best| x filled, raw at 10^9 in quote units
};

template <typename L>
concept Level = requires(const L& l) {
  { l.price } -> std::convertible_to<model::Price>;
  { l.size } -> std::convertible_to<model::Quantity>;
};

template <typename S, typename L>
concept SlippageModel =
    Level<L> && requires(const S& s, model::OrderSide side, std::span<const L> levels,
                         model::Quantity quantity, SlippageEstimate& out) {
      { s.estimate(side, levels, quantity, out) } -> std::same_as<core::Status>;
    };

class BookDepthSlippage {
public:
  // `side` is the taker's side: a BUY walks the asks. InvalidArgument without levels or for a
  // zero quantity.
  template <Level L>
  [[nodiscard]] static constexpr core::Status
  estimate(model::OrderSide side, std::span<const L> levels, model::Quantity quantity,
           SlippageEstimate& out) noexcept {
    if (levels.empty() || quantity.is_zero()) {
      return core::Status::InvalidArgument;
    }
    const model::Price best = levels.front().price;
    core::u128 notional = 0; // sum of price.raw x size.raw (10^18 scale)
    std::uint64_t filled = 0;
    const std::uint64_t want = quantity.raw();
    for (const L& level : levels) {
      if (filled >= want) {
        break;
      }
      const std::uint64_t size = level.size.raw();
      const std::uint64_t take = size < want - filled ? size : want - filled;
      notional += static_cast<core::u128>(core::magnitude(level.price.raw())) * take;
      filled += take;
    }
    if (filled == 0) {
      return core::Status::InvalidArgument;
    }
    // Round the average against the taker: up for a buy, down for a sell.
    const std::uint64_t unit = core::kPow10[model::kFixedPrecision - best.precision()];
    const core::u128 per_unit = static_cast<core::u128>(filled) * unit;
    core::u128 steps = notional / per_unit;
    if (side == model::OrderSide::Buy && steps * per_unit != notional) {
      ++steps;
    }
    const auto average_raw = static_cast<std::int64_t>(steps * unit);
    core::Status s = model::Price::from_raw(average_raw, best.precision(), out.average);
    if (!core::ok(s)) {
      return s;
    }
    s = model::Quantity::from_raw(filled, quantity.precision(), out.filled);
    if (!core::ok(s)) {
      return s;
    }
    out.best = best;
    out.complete = filled == want;
    const std::uint64_t gap = core::magnitude(average_raw - best.raw());
    out.cost_raw = static_cast<std::int64_t>(static_cast<core::u128>(gap) * filled /
                                             core::kPow10[model::kFixedPrecision]);
    return core::Status::Ok;
  }
};

} // namespace jarvis::cost
