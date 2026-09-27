#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"

// The order management system (docs/architecture.md sections 8.3, 8.5 and 9.3): every order the
// node created, found by ClientOrderId, with its state (OrderState), its fills and the
// strategy it belongs to. Capacities are fixed at construction:
//
//   - orders: when the arena is full, the oldest closed order is evicted to make room; with no
//     closed order left, create() returns CapacityExceeded and the caller denies the order;
//   - trades: the fill records used to refuse a second fill of one trade (Binance sends
//     TRADE_LITE and ORDER_TRADE_UPDATE for the same trade); an evicted order frees its records.
//
// Netting accounts only (one-way mode, section 8.5); hedging arrives with v1.x.

namespace jarvis::execution {

inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFU;

struct OrderRecord {
  model::ClientOrderId client_order_id;
  std::optional<model::VenueOrderId> venue_order_id;
  std::uint16_t strategy = 0;
  std::uint32_t slot = 0; // instrument slot
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::Buy;
  model::OrderType type = model::OrderType::Limit;
  model::TimeInForce time_in_force = model::TimeInForce::Gtc;
  bool post_only = false;
  bool reduce_only = false;
  std::optional<model::Price> price;
  OrderState state;
  core::i128 fill_notional = 0; // sum of last_px.raw * last_qty.raw (10^18 scale)
  core::UnixNanos ts_init;
  std::uint32_t trades = kNoIndex; // head of this order's trade list
  std::uint32_t parent = kNoIndex; // the execution algorithm's parent slot, for child orders
  std::uint64_t parent_seq = 0;    // and that parent's ClientOrderId sequence number (the slot
                                   // is reused once the parent closes)
  bool used = false;
};

// Open order quantities of one instrument, for open_exposure() (section 9.3) and the risk gates.
struct OpenQuantity {
  std::uint64_t buy_raw = 0;    // leaves of open buy orders
  std::uint64_t sell_raw = 0;   // leaves of open sell orders
  std::uint32_t orders = 0;     // open orders
  core::u128 buy_notional = 0;  // sum of leaves.raw x price.raw of open priced buy orders
  core::u128 sell_notional = 0; // the same for sells (10^18 scale)
};

class Oms {
public:
  // `slots` instruments get running open totals (open_quantity in O(1)); others are scanned.
  Oms(std::uint32_t orders, std::uint32_t trades, std::uint32_t slots = 0)
      : orders_{orders}, trades_{trades}, table_{table_size(orders)}, closed_{orders},
        open_{slots} {
    for (std::uint32_t i = 0; i < slots; ++i) {
      static_cast<void>(open_.push_back(OpenQuantity{}));
    }
    for (std::uint32_t i = 0; i < orders; ++i) {
      static_cast<void>(orders_.push_back(OrderRecord{}));
      static_cast<void>(closed_.push_back(kNoIndex));
    }
    for (std::uint32_t i = 0; i < trades; ++i) {
      static_cast<void>(trades_.push_back(TradeRecord{{}, 0, i + 1 < trades ? i + 1 : kNoIndex}));
    }
    free_trade_ = trades > 0 ? 0 : kNoIndex;
    for (std::size_t i = 0; i < table_.capacity(); ++i) {
      static_cast<void>(table_.push_back(kNoIndex));
    }
  }

  // Registers a new order (status INITIALIZED); `index` addresses it until it is evicted.
  [[nodiscard]] core::Status create(const OrderRecord& record, std::uint32_t& index) noexcept {
    if (find(record.client_order_id) != kNoIndex) {
      return core::Status::AlreadyExists;
    }
    std::uint32_t i = kNoIndex;
    if (next_unused_ < orders_.size()) {
      i = next_unused_++;
    } else {
      const core::Status s = evict(i);
      if (!core::ok(s)) {
        return s;
      }
    }
    OrderRecord& r = orders_[i];
    r = record;
    r.used = true;
    r.trades = kNoIndex;
    r.fill_notional = 0;
    insert(i);
    index = i;
    ++live_;
    return core::Status::Ok;
  }

  [[nodiscard]] std::uint32_t find(const model::ClientOrderId& id) const noexcept {
    const std::size_t mask = table_.size() - 1;
    for (std::size_t probe = hash(id) & mask;; probe = (probe + 1) & mask) {
      const std::uint32_t i = table_[probe];
      if (i == kNoIndex) {
        return kNoIndex;
      }
      if (orders_[i].client_order_id == id) {
        return i;
      }
    }
  }

  [[nodiscard]] OrderRecord& at(std::uint32_t index) noexcept { return orders_[index]; }
  [[nodiscard]] const OrderRecord& at(std::uint32_t index) const noexcept { return orders_[index]; }
  [[nodiscard]] std::uint32_t live() const noexcept { return live_; }
  // Indices below this bound have been used at least once (iterate with at(i).used).
  [[nodiscard]] std::uint32_t used_bound() const noexcept { return next_unused_; }

  // A plain event (no quantity). Records the order as closed when it becomes so.
  [[nodiscard]] core::Status apply(std::uint32_t index, OrderEventKind kind) noexcept {
    OrderRecord& r = orders_[index];
    const bool was_closed = is_closed(r.state.status());
    const Share before = share(r);
    const core::Status s = r.state.apply(kind);
    track(r, before);
    note_closed(index, was_closed);
    return s;
  }

  [[nodiscard]] core::Status update(std::uint32_t index, model::Quantity quantity,
                                    std::optional<model::Price> price) noexcept {
    OrderRecord& r = orders_[index];
    const Share before = share(r);
    const core::Status s = r.state.update(quantity);
    if (core::ok(s) && price) {
      r.price = price;
    }
    track(r, before);
    return s;
  }

  // A fill of `trade_id`; DuplicateFill when this order already has that trade. A Lite fill
  // (Binance TRADE_LITE) comes without its commission, which a later report brings.
  [[nodiscard]] core::Status fill(std::uint32_t index, const model::TradeId& trade_id,
                                  model::Quantity qty, model::Price px,
                                  bool commission_pending = false) noexcept {
    OrderRecord& r = orders_[index];
    if (find_trade(r, trade_id) != kNoIndex) {
      return core::Status::DuplicateFill;
    }
    if (free_trade_ == kNoIndex) {
      return core::Status::CapacityExceeded;
    }
    const bool was_closed = is_closed(r.state.status());
    const Share before = share(r);
    const core::Status s = r.state.fill(qty);
    if (!core::ok(s)) {
      return s;
    }
    track(r, before);
    const std::uint32_t t = free_trade_;
    free_trade_ = trades_[t].next;
    trades_[t] = TradeRecord{trade_id, qty.raw(), r.trades, commission_pending};
    r.trades = t;
    r.fill_notional += static_cast<core::i128>(px.raw()) * static_cast<core::i128>(qty.raw());
    note_closed(index, was_closed);
    return core::Status::Ok;
  }

  // True once for a Lite fill of `trade_id` when a later report of it brings the commission
  // (docs/architecture.md section 8.3); false for any other trade or a second report.
  [[nodiscard]] bool take_pending_commission(std::uint32_t index,
                                             const model::TradeId& trade_id) noexcept {
    const std::uint32_t t = find_trade(orders_[index], trade_id);
    if (t == kNoIndex || !trades_[t].commission_pending) {
      return false;
    }
    trades_[t].commission_pending = false;
    return true;
  }

  // Voids `voided` of an earlier fill of `trade_id` at `px`.
  [[nodiscard]] core::Status void_fill(std::uint32_t index, const model::TradeId& trade_id,
                                       model::Quantity voided, model::Price px) noexcept {
    OrderRecord& r = orders_[index];
    const std::uint32_t t = find_trade(r, trade_id);
    if (t == kNoIndex || voided.raw() > trades_[t].qty_raw) {
      return core::Status::InvalidArgument;
    }
    const bool was_closed = is_closed(r.state.status());
    const Share before = share(r);
    const core::Status s = r.state.void_fill(voided);
    if (!core::ok(s)) {
      return s;
    }
    track(r, before);
    trades_[t].qty_raw -= voided.raw();
    r.fill_notional -= static_cast<core::i128>(px.raw()) * static_cast<core::i128>(voided.raw());
    note_closed(index, was_closed);
    return core::Status::Ok;
  }

  // Average fill price at the order's price precision (truncated), or false without fills.
  [[nodiscard]] static bool average_price(const OrderRecord& r, std::uint8_t precision,
                                          model::Price& out) noexcept {
    const std::uint64_t filled = r.state.filled().raw();
    if (filled == 0) {
      return false;
    }
    if (precision > model::kFixedPrecision) {
      return false;
    }
    core::i128 avg = r.fill_notional / static_cast<core::i128>(filled);
    const auto unit = static_cast<core::i128>(core::kPow10[model::kFixedPrecision - precision]);
    avg -= avg % unit;
    return core::ok(model::Price::from_raw(static_cast<std::int64_t>(avg), precision, out));
  }

  // Leaves of the open orders of instrument `slot` (all strategies, or one).
  [[nodiscard]] OpenQuantity
  open_quantity(std::uint32_t slot, std::optional<std::uint16_t> strategy = {}) const noexcept {
    if (!strategy && slot < open_.size()) {
      return open_[slot];
    }
    OpenQuantity out;
    for (std::uint32_t i = 0; i < next_unused_; ++i) {
      const OrderRecord& r = orders_[i];
      if (!r.used || r.slot != slot || !is_open(r.state.status()) ||
          (strategy && r.strategy != *strategy)) {
        continue;
      }
      const Share sh = share(r);
      (r.side == model::OrderSide::Buy ? out.buy_raw : out.sell_raw) += sh.leaves;
      (r.side == model::OrderSide::Buy ? out.buy_notional : out.sell_notional) += sh.notional;
      ++out.orders;
    }
    return out;
  }

private:
  // What an order adds to its instrument's open totals.
  struct Share {
    std::uint64_t leaves = 0;
    core::u128 notional = 0;
    bool open = false;
  };

  [[nodiscard]] static Share share(const OrderRecord& r) noexcept {
    if (!r.used || !is_open(r.state.status())) {
      return {};
    }
    const std::uint64_t leaves = r.state.leaves().raw();
    const core::u128 notional =
        r.price ? static_cast<core::u128>(core::magnitude(r.price->raw())) * leaves : 0;
    return Share{leaves, notional, true};
  }

  void track(const OrderRecord& r, const Share& before) noexcept {
    if (r.slot >= open_.size()) {
      return;
    }
    const Share after = share(r);
    OpenQuantity& o = open_[r.slot];
    const bool buy = r.side == model::OrderSide::Buy;
    std::uint64_t& q = buy ? o.buy_raw : o.sell_raw;
    core::u128& n = buy ? o.buy_notional : o.sell_notional;
    q = q - before.leaves + after.leaves;
    n = n - before.notional + after.notional;
    o.orders = o.orders - (before.open ? 1U : 0U) + (after.open ? 1U : 0U);
  }

  struct TradeRecord {
    model::TradeId trade_id;
    std::uint64_t qty_raw = 0;
    std::uint32_t next = kNoIndex;
    bool commission_pending = false; // a Lite fill whose commission has not been reported
  };

  static std::size_t table_size(std::uint32_t orders) noexcept {
    std::size_t n = 16;
    while (n < static_cast<std::size_t>(orders) * 2) {
      n *= 2;
    }
    return n;
  }

  static std::size_t hash(const model::ClientOrderId& id) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : id.view()) {
      h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(h ^ (h >> 32U));
  }

  void insert(std::uint32_t index) noexcept {
    const std::size_t mask = table_.size() - 1;
    std::size_t probe = hash(orders_[index].client_order_id) & mask;
    while (table_[probe] != kNoIndex) {
      probe = (probe + 1) & mask;
    }
    table_[probe] = index;
  }

  // Linear probing with backward-shift deletion: no tombstones.
  void erase(std::uint32_t index) noexcept {
    const std::size_t mask = table_.size() - 1;
    std::size_t hole = hash(orders_[index].client_order_id) & mask;
    while (table_[hole] != index) {
      hole = (hole + 1) & mask;
    }
    std::size_t next = (hole + 1) & mask;
    while (table_[next] != kNoIndex) {
      const std::size_t home = hash(orders_[table_[next]].client_order_id) & mask;
      // Move the entry back when its home is not in (hole, next] (cyclically).
      const bool in_range =
          hole <= next ? (home > hole && home <= next) : (home > hole || home <= next);
      if (!in_range) {
        table_[hole] = table_[next];
        hole = next;
      }
      next = (next + 1) & mask;
    }
    table_[hole] = kNoIndex;
  }

  [[nodiscard]] std::uint32_t find_trade(const OrderRecord& r,
                                         const model::TradeId& id) const noexcept {
    for (std::uint32_t t = r.trades; t != kNoIndex; t = trades_[t].next) {
      if (trades_[t].trade_id == id) {
        return t;
      }
    }
    return kNoIndex;
  }

  // A closed order never reopens (the transition table has no edge from a closed status to an
  // open one), so each order enters the ring at most once and `orders` slots suffice.
  void note_closed(std::uint32_t index, bool was_closed) noexcept {
    if (!was_closed && is_closed(orders_[index].state.status()) && closed_count_ < closed_.size()) {
      closed_[(closed_head_ + closed_count_) % closed_.size()] = index;
      ++closed_count_;
    }
  }

  // Frees the oldest closed order.
  [[nodiscard]] core::Status evict(std::uint32_t& index) noexcept {
    while (closed_count_ > 0) {
      const std::uint32_t i = closed_[closed_head_];
      closed_head_ = (closed_head_ + 1) % closed_.size();
      --closed_count_;
      if (orders_[i].used && is_closed(orders_[i].state.status())) {
        release(i);
        index = i;
        return core::Status::Ok;
      }
    }
    return core::Status::CapacityExceeded;
  }

  void release(std::uint32_t i) noexcept {
    OrderRecord& r = orders_[i];
    erase(i);
    std::uint32_t t = r.trades;
    while (t != kNoIndex) {
      const std::uint32_t next = trades_[t].next;
      trades_[t].next = free_trade_;
      free_trade_ = t;
      t = next;
    }
    r = OrderRecord{};
    --live_;
  }

  core::FixedVector<OrderRecord> orders_;
  core::FixedVector<TradeRecord> trades_;
  core::FixedVector<std::uint32_t> table_;
  core::FixedVector<std::uint32_t> closed_; // ring of closed orders, oldest at closed_head_
  core::FixedVector<OpenQuantity> open_;    // running open totals by instrument slot
  std::size_t closed_head_ = 0;
  std::size_t closed_count_ = 0;
  std::uint32_t next_unused_ = 0;
  std::uint32_t free_trade_ = kNoIndex;
  std::uint32_t live_ = 0;
};

} // namespace jarvis::execution
