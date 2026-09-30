#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/state.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"

// Price-level order books (docs/architecture.md section 7.5). Prices are indexed by tick
// (price.raw / price_increment.raw), so a level lookup is an array access. Levels inside a dense
// window of `window_levels` ticks live in arrays with an occupancy bitmap (finding the next
// level is a count-leading-zeros over 64-level words); levels outside the window live in a
// small sorted overflow list, so the book stays exact when prices move far. The window recentres
// on the mid when the touch leaves its middle half. All storage is sized at construction.

namespace jarvis::data {

struct BookLevel {
  model::Price price;
  model::Quantity size;
};

struct BookConfig {
  model::BookType type = model::BookType::L2_MBP;
  model::Price tick; // the instrument's price increment
  std::uint8_t size_precision = 0;
  std::uint32_t window_levels = 16384;  // rounded up to a multiple of 64
  std::uint32_t overflow_levels = 4096; // per side
};

class OrderBook {
public:
  // An empty shell for a snapshot to restore into (state() gives it its shape).
  OrderBook() = default;
  explicit OrderBook(const BookConfig& config)
      : type_{config.type}, tick_raw_{config.tick.raw()}, price_precision_{config.tick.precision()},
        size_precision_{config.size_precision}, window_{(config.window_levels + 63U) / 64U * 64U},
        bids_{window_}, asks_{window_}, bid_bits_{window_ / 64U}, ask_bits_{window_ / 64U},
        bid_overflow_{config.overflow_levels}, ask_overflow_{config.overflow_levels} {
    for (std::uint32_t i = 0; i < window_; ++i) {
      static_cast<void>(bids_.push_back(0));
      static_cast<void>(asks_.push_back(0));
    }
    for (std::uint32_t i = 0; i < window_ / 64U; ++i) {
      static_cast<void>(bid_bits_.push_back(0));
      static_cast<void>(ask_bits_.push_back(0));
    }
  }

  // Applies one delta (L2_MBP): Add and Update set the level's size, Delete removes it, Clear
  // empties the book (the start of a snapshot).
  [[nodiscard]] core::Status apply(const model::OrderBookDelta& d) noexcept {
    if (type_ != model::BookType::L2_MBP) {
      return core::Status::InvalidState;
    }
    sequence_ = d.sequence;
    ts_last_ = d.ts_event;
    if (d.action == model::BookAction::Clear) {
      clear();
      return core::Status::Ok;
    }
    if (!d.order.side) {
      return core::Status::InvalidArgument;
    }
    std::int64_t tick = 0;
    core::Status s = tick_of(d.order.price, tick);
    if (!core::ok(s)) {
      return s;
    }
    const Side side = *d.order.side == model::OrderSide::Buy ? Side::Bid : Side::Ask;
    const std::uint64_t size = d.action == model::BookAction::Delete ? 0 : d.order.size.raw();
    s = set_level(side, tick, size);
    if (!core::ok(s)) {
      return s;
    }
    return maybe_recentre();
  }

  // Applies a batch of deltas in order; stops at the first failure.
  [[nodiscard]] core::Status apply(const model::OrderBookDeltas& deltas) noexcept {
    for (const model::OrderBookDelta& d : deltas.deltas) {
      const core::Status s = apply(d);
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  // L1_MBP: the quote replaces the top level of each side.
  [[nodiscard]] core::Status apply(const model::QuoteTick& q) noexcept {
    if (type_ != model::BookType::L1_MBP) {
      return core::Status::InvalidState;
    }
    std::int64_t bid = 0;
    std::int64_t ask = 0;
    core::Status s = tick_of(q.bid_price, bid);
    if (core::ok(s)) {
      s = tick_of(q.ask_price, ask);
    }
    if (!core::ok(s)) {
      return s;
    }
    clear_side_top(Side::Bid);
    clear_side_top(Side::Ask);
    s = set_level(Side::Bid, bid, q.bid_size.raw());
    if (core::ok(s)) {
      s = set_level(Side::Ask, ask, q.ask_size.raw());
    }
    ts_last_ = q.ts_event;
    return core::ok(s) ? maybe_recentre() : s;
  }

  void clear() noexcept {
    for (std::uint32_t w = 0; w < window_ / 64U; ++w) {
      clear_word(bids_, bid_bits_, w);
      clear_word(asks_, ask_bits_, w);
    }
    bid_overflow_.clear();
    ask_overflow_.clear();
    has_base_ = false;
  }

  [[nodiscard]] bool best_bid(BookLevel& out) const noexcept { return best(Side::Bid, out); }
  [[nodiscard]] bool best_ask(BookLevel& out) const noexcept { return best(Side::Ask, out); }

  // Writes up to out.size() levels from the touch outwards; returns how many.
  [[nodiscard]] std::size_t bids(std::span<BookLevel> out) const noexcept {
    return levels(Side::Bid, out);
  }
  [[nodiscard]] std::size_t asks(std::span<BookLevel> out) const noexcept {
    return levels(Side::Ask, out);
  }

  // Size at an exact price; zero when the level is empty.
  [[nodiscard]] model::Quantity size_at(model::OrderSide side, model::Price price) const noexcept {
    std::int64_t tick = 0;
    if (!core::ok(tick_of(price, tick))) {
      return quantity(0);
    }
    return quantity(level_size(side == model::OrderSide::Buy ? Side::Bid : Side::Ask, tick));
  }

  [[nodiscard]] model::BookType type() const noexcept { return type_; }
  [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
  [[nodiscard]] core::UnixNanos ts_last() const noexcept { return ts_last_; }
  [[nodiscard]] std::uint8_t price_precision() const noexcept { return price_precision_; }
  [[nodiscard]] std::uint8_t size_precision() const noexcept { return size_precision_; }

private:
  enum class Side : std::uint8_t { Bid, Ask };

  struct OverflowLevel {
    std::int64_t tick = 0;
    std::uint64_t size = 0;

    template <typename Ar> void state(Ar& ar) { ar(tick, size); }
  };

  [[nodiscard]] core::Status tick_of(model::Price price, std::int64_t& tick) const noexcept {
    if (tick_raw_ <= 0 || price.raw() % tick_raw_ != 0) {
      return core::Status::InvalidArgument; // not on the instrument's price grid
    }
    tick = price.raw() / tick_raw_;
    return core::Status::Ok;
  }

  [[nodiscard]] model::Price price(std::int64_t tick) const noexcept {
    model::Price p;
    static_cast<void>(model::Price::from_raw(tick * tick_raw_, price_precision_, p));
    return p;
  }
  [[nodiscard]] model::Quantity quantity(std::uint64_t raw) const noexcept {
    model::Quantity q;
    static_cast<void>(model::Quantity::from_raw(raw, size_precision_, q));
    return q;
  }

  [[nodiscard]] bool in_window(std::int64_t tick) const noexcept {
    return has_base_ && tick >= base_ && tick - base_ < static_cast<std::int64_t>(window_);
  }

  [[nodiscard]] core::FixedVector<std::uint64_t>& sizes(Side s) noexcept {
    return s == Side::Bid ? bids_ : asks_;
  }
  [[nodiscard]] const core::FixedVector<std::uint64_t>& sizes(Side s) const noexcept {
    return s == Side::Bid ? bids_ : asks_;
  }
  [[nodiscard]] core::FixedVector<std::uint64_t>& bits(Side s) noexcept {
    return s == Side::Bid ? bid_bits_ : ask_bits_;
  }
  [[nodiscard]] const core::FixedVector<std::uint64_t>& bits(Side s) const noexcept {
    return s == Side::Bid ? bid_bits_ : ask_bits_;
  }
  [[nodiscard]] core::FixedVector<OverflowLevel>& overflow(Side s) noexcept {
    return s == Side::Bid ? bid_overflow_ : ask_overflow_;
  }
  [[nodiscard]] const core::FixedVector<OverflowLevel>& overflow(Side s) const noexcept {
    return s == Side::Bid ? bid_overflow_ : ask_overflow_;
  }

  static void clear_word(core::FixedVector<std::uint64_t>& sizes,
                         core::FixedVector<std::uint64_t>& bits, std::uint32_t w) noexcept {
    std::uint64_t word = bits[w];
    while (word != 0) {
      const auto bit = static_cast<std::uint32_t>(__builtin_ctzll(word));
      sizes[w * 64U + bit] = 0;
      word &= word - 1;
    }
    bits[w] = 0;
  }

  [[nodiscard]] std::uint64_t level_size(Side s, std::int64_t tick) const noexcept {
    if (in_window(tick)) {
      return sizes(s)[static_cast<std::size_t>(tick - base_)];
    }
    const core::FixedVector<OverflowLevel>& list = overflow(s);
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (list[i].tick == tick) {
        return list[i].size;
      }
    }
    return 0;
  }

  // Sets (size > 0) or removes (size == 0) a level.
  [[nodiscard]] core::Status set_level(Side s, std::int64_t tick, std::uint64_t size) noexcept {
    if (!has_base_) {
      if (size == 0) {
        return core::Status::Ok;
      }
      base_ = tick - static_cast<std::int64_t>(window_ / 2U);
      has_base_ = true;
    }
    if (in_window(tick)) {
      const auto i = static_cast<std::size_t>(tick - base_);
      sizes(s)[i] = size;
      const std::uint64_t mask = std::uint64_t{1} << (i % 64U);
      bits(s)[i / 64U] = size == 0 ? (bits(s)[i / 64U] & ~mask) : (bits(s)[i / 64U] | mask);
      return core::Status::Ok;
    }
    return set_overflow(s, tick, size);
  }

  // Overflow levels are kept sorted by tick, ascending.
  [[nodiscard]] core::Status set_overflow(Side s, std::int64_t tick, std::uint64_t size) noexcept {
    core::FixedVector<OverflowLevel>& list = overflow(s);
    std::size_t i = 0;
    while (i < list.size() && list[i].tick < tick) {
      ++i;
    }
    if (i < list.size() && list[i].tick == tick) {
      if (size != 0) {
        list[i].size = size;
        return core::Status::Ok;
      }
      for (std::size_t j = i + 1; j < list.size(); ++j) {
        list[j - 1] = list[j];
      }
      list.pop_back();
      return core::Status::Ok;
    }
    if (size == 0) {
      return core::Status::Ok;
    }
    const core::Status pushed = list.push_back(OverflowLevel{});
    if (!core::ok(pushed)) {
      return pushed;
    }
    for (std::size_t j = list.size() - 1; j > i; --j) {
      list[j] = list[j - 1];
    }
    list[i] = OverflowLevel{tick, size};
    return core::Status::Ok;
  }

  void clear_side_top(Side s) noexcept {
    std::int64_t tick = 0;
    if (best_tick(s, tick)) {
      static_cast<void>(set_level(s, tick, 0));
    }
  }

  // Highest occupied dense index at or below `from` (bids), or lowest at or above (asks).
  [[nodiscard]] bool dense_next(Side s, std::int64_t from, bool downward,
                                std::int64_t& index) const noexcept {
    const core::FixedVector<std::uint64_t>& b = bits(s);
    const auto words = static_cast<std::int64_t>(window_ / 64U);
    if (from < 0 || from >= static_cast<std::int64_t>(window_)) {
      return false;
    }
    std::int64_t w = from / 64;
    const auto bit = static_cast<std::uint32_t>(from % 64);
    std::uint64_t word = b[static_cast<std::size_t>(w)];
    if (!downward) {
      word &= ~((std::uint64_t{1} << bit) - 1U);
    } else if (bit != 63U) {
      word &= (std::uint64_t{1} << (bit + 1U)) - 1U;
    }
    while (true) {
      if (word != 0) {
        const int pos = downward ? 63 - __builtin_clzll(word) : __builtin_ctzll(word);
        index = w * 64 + pos;
        return true;
      }
      w += downward ? -1 : 1;
      if (w < 0 || w >= words) {
        return false;
      }
      word = b[static_cast<std::size_t>(w)];
    }
  }

  [[nodiscard]] bool best_tick(Side s, std::int64_t& tick) const noexcept {
    bool found = false;
    const bool bid = s == Side::Bid;
    std::int64_t index = 0;
    if (has_base_ && dense_next(s, bid ? static_cast<std::int64_t>(window_) - 1 : 0, bid, index)) {
      tick = base_ + index;
      found = true;
    }
    const core::FixedVector<OverflowLevel>& list = overflow(s);
    if (!list.empty()) {
      const std::int64_t candidate = bid ? list.back().tick : list[0].tick;
      if (!found || (bid ? candidate > tick : candidate < tick)) {
        tick = candidate;
        found = true;
      }
    }
    return found;
  }

  [[nodiscard]] bool best(Side s, BookLevel& out) const noexcept {
    std::int64_t tick = 0;
    if (!best_tick(s, tick)) {
      return false;
    }
    out = BookLevel{price(tick), quantity(level_size(s, tick))};
    return true;
  }

  // Merges dense and overflow levels from the touch outwards.
  [[nodiscard]] std::size_t levels(Side s, std::span<BookLevel> out) const noexcept {
    const bool bid = s == Side::Bid;
    const core::FixedVector<OverflowLevel>& list = overflow(s);
    // Overflow cursor: bids walk from the highest tick down, asks from the lowest up.
    std::size_t o = 0;
    std::size_t n = 0;
    std::int64_t dense_index = 0;
    bool dense_ok = has_base_ && dense_next(s, bid ? static_cast<std::int64_t>(window_) - 1 : 0,
                                            bid, dense_index);
    while (n < out.size()) {
      const bool overflow_ok = o < list.size();
      if (!dense_ok && !overflow_ok) {
        break;
      }
      std::int64_t overflow_tick = 0;
      if (overflow_ok) {
        overflow_tick = bid ? list[list.size() - 1 - o].tick : list[o].tick;
      }
      const std::int64_t dense_tick = base_ + dense_index;
      const bool take_dense =
          dense_ok &&
          (!overflow_ok || (bid ? dense_tick > overflow_tick : dense_tick < overflow_tick));
      if (take_dense) {
        out[n++] =
            BookLevel{price(dense_tick), quantity(sizes(s)[static_cast<std::size_t>(dense_index)])};
        dense_ok = dense_next(s, dense_index + (bid ? -1 : 1), bid, dense_index);
      } else {
        const OverflowLevel& l = bid ? list[list.size() - 1 - o] : list[o];
        out[n++] = BookLevel{price(l.tick), quantity(l.size)};
        ++o;
      }
    }
    return n;
  }

  // Recentres the window on the mid when the touch leaves the middle half of the window.
  [[nodiscard]] core::Status maybe_recentre() noexcept {
    std::int64_t bid = 0;
    std::int64_t ask = 0;
    const bool has_bid = best_tick(Side::Bid, bid);
    const bool has_ask = best_tick(Side::Ask, ask);
    if (!has_bid && !has_ask) {
      return core::Status::Ok;
    }
    std::int64_t mid = has_bid ? bid : ask;
    if (has_bid && has_ask) {
      mid = bid + (ask - bid) / 2;
    }
    const auto quarter = static_cast<std::int64_t>(window_ / 4U);
    if (mid >= base_ + quarter && mid < base_ + 3 * quarter) {
      return core::Status::Ok;
    }
    return recentre(mid - static_cast<std::int64_t>(window_ / 2U));
  }

  [[nodiscard]] core::Status recentre(std::int64_t new_base) noexcept {
    for (const Side s : {Side::Bid, Side::Ask}) {
      // Move every dense level to the overflow list, then pull back those inside the new window.
      for (std::uint32_t w = 0; w < window_ / 64U; ++w) {
        std::uint64_t word = bits(s)[w];
        while (word != 0) {
          const auto bit = static_cast<std::uint32_t>(__builtin_ctzll(word));
          const std::uint32_t i = w * 64U + bit;
          const core::Status st =
              set_overflow(s, base_ + static_cast<std::int64_t>(i), sizes(s)[i]);
          if (!core::ok(st)) {
            return st;
          }
          word &= word - 1;
        }
        clear_word(sizes(s), bits(s), w);
      }
    }
    base_ = new_base;
    for (const Side s : {Side::Bid, Side::Ask}) {
      core::FixedVector<OverflowLevel>& list = overflow(s);
      std::size_t keep = 0;
      for (std::size_t i = 0; i < list.size(); ++i) {
        const OverflowLevel l = list[i];
        if (in_window(l.tick)) {
          static_cast<void>(set_level(s, l.tick, l.size));
        } else {
          list[keep++] = l;
        }
      }
      while (list.size() > keep) {
        list.pop_back();
      }
    }
    return core::Status::Ok;
  }

  model::BookType type_ = model::BookType::L2_MBP;
  std::int64_t tick_raw_ = 0;
  std::uint8_t price_precision_ = 0;
  std::uint8_t size_precision_ = 0;
  std::uint32_t window_ = 0;
  core::FixedVector<std::uint64_t> bids_{0};
  core::FixedVector<std::uint64_t> asks_{0};
  core::FixedVector<std::uint64_t> bid_bits_{0};
  core::FixedVector<std::uint64_t> ask_bits_{0};
  core::FixedVector<OverflowLevel> bid_overflow_{0};
  core::FixedVector<OverflowLevel> ask_overflow_{0};
  std::int64_t base_ = 0;
  bool has_base_ = false;
  std::uint64_t sequence_ = 0;
  core::UnixNanos ts_last_;

public:
  // Snapshot encoding (core/state.hpp): the shape, then the levels, the window's mostly empty
  // arrays as runs.
  template <typename Ar> void state(Ar& ar) {
    ar(type_, tick_raw_, price_precision_, size_precision_, window_);
    if constexpr (Ar::kReading) {
      if (window_ % 64U != 0 || window_ > (1U << 24U)) {
        ar.fail(core::Status::InvalidArgument);
        return;
      }
      if (bids_.capacity() != window_) {
        shape(window_);
      }
    }
    core::state_sparse(ar, bids_);
    core::state_sparse(ar, asks_);
    core::state_sparse(ar, bid_bits_);
    core::state_sparse(ar, ask_bits_);
    ar(bid_overflow_, ask_overflow_, base_, has_base_, sequence_, ts_last_);
  }

private:
  // Empty level arrays for a window of `window` levels (a restore into the empty shell).
  void shape(std::uint32_t window) {
    bids_ = core::FixedVector<std::uint64_t>{window};
    asks_ = core::FixedVector<std::uint64_t>{window};
    bid_bits_ = core::FixedVector<std::uint64_t>{window / 64U};
    ask_bits_ = core::FixedVector<std::uint64_t>{window / 64U};
    for (std::uint32_t i = 0; i < window; ++i) {
      static_cast<void>(bids_.push_back(0));
      static_cast<void>(asks_.push_back(0));
    }
    for (std::uint32_t i = 0; i < window / 64U; ++i) {
      static_cast<void>(bid_bits_.push_back(0));
      static_cast<void>(ask_bits_.push_back(0));
    }
  }
};

// Read-only view of one instrument's book, valid during the callback that receives it.
// Python receives copies of its levels, never the view.
struct BookView {
  model::InstrumentId instrument_id;
  const OrderBook* book = nullptr;

  [[nodiscard]] bool best_bid(BookLevel& out) const noexcept { return book->best_bid(out); }
  [[nodiscard]] bool best_ask(BookLevel& out) const noexcept { return book->best_ask(out); }
  [[nodiscard]] std::size_t bids(std::span<BookLevel> out) const noexcept {
    return book->bids(out);
  }
  [[nodiscard]] std::size_t asks(std::span<BookLevel> out) const noexcept {
    return book->asks(out);
  }
  [[nodiscard]] std::uint64_t sequence() const noexcept { return book->sequence(); }
  [[nodiscard]] core::UnixNanos ts_last() const noexcept { return book->ts_last(); }
};

} // namespace jarvis::data
