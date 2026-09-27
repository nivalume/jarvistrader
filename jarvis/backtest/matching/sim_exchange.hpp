#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/cost/fees.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/portfolio/margin.hpp"
#include "jarvis/portfolio/portfolio.hpp"

// The simulated venue (docs/architecture.md sections 12.1 and 12.3). It sees market data and
// commands at venue time and answers with the order events Binance would send: accepted,
// rejected, filled, canceled, expired, updated, and the rejections of modifies and cancels.
//
// Market state per instrument: the top of book from quotes, and an L2 book once deltas arrive
// (L2 then takes precedence). Our orders are not part of that book; they match against it.
//
//   taker    an order that crosses on arrival (or after a modify) takes the opposite side, best
//            level first: L2 levels up to its limit, or the top of book capped by its size.
//            IOC and market remainders expire, FOK expires unless it fills completely, GTC and
//            GTD remainders rest. A post-only (GTX) order that would take is rejected (-5022).
//   maker    a resting order fills at its own price:
//              TopOfBook      when the opposite best reaches its price (up to that size), or a
//                             trade prints through it (up to the trade's size);
//              QueuePosition  as TopOfBook, and also at its own price once the volume ahead of
//                             it in the queue has traded: trades at the price consume the queue
//                             ahead, size decreases at the price shrink it in proportion, size
//                             increases join behind it.
//   STP      an arriving order that would cross one of our resting orders on the other side
//            expires (EXPIRE_TAKER), expires those resting orders (EXPIRE_MAKER), or both.
//   fees     from the FeeModel, in the settlement currency, on every fill.
//   GTD      orders expire at the first venue event at or after their expire time.
//
// The venue keeps its own account (positions and balances, a Portfolio) for reduce-only checks
// and snapshots.

namespace jarvis::backtest {

enum class FillModel : std::uint8_t { TopOfBook = 0, QueuePosition = 1 };
enum class StpMode : std::uint8_t { None = 0, ExpireTaker = 1, ExpireMaker = 2, ExpireBoth = 3 };

struct SimConfig {
  FillModel fill_model = FillModel::TopOfBook;
  StpMode stp = StpMode::None;
  cost::MakerTakerFees fees; // zero unless set
  std::uint32_t instruments = 64;
  std::uint32_t strategies = 8;
  std::uint32_t orders = 4096;               // resting orders
  std::uint32_t book_levels = 4096;          // L2 window per instrument, in ticks
  std::uint32_t book_overflow_levels = 1024; // L2 levels per side outside the window
  std::uint32_t walk_levels = 64;            // levels one taker order may consume
  std::uint32_t events = 4096;               // venue events per call
  portfolio::Margin margin = portfolio::StandardMargin{};
  model::AccountId account_id;
  model::TraderId trader_id;
  std::uint64_t seed = 0;
};

namespace reject {
inline constexpr std::string_view kUnknownSymbol = "-1121 INVALID_SYMBOL";
inline constexpr std::string_view kPostOnly = "-5022 POST_ONLY_WOULD_TAKE";
inline constexpr std::string_view kReduceOnly = "-2022 REDUCE_ONLY_REJECTED";
inline constexpr std::string_view kNoMarket = "NO_MARKET";
inline constexpr std::string_view kCapacity = "VENUE_CAPACITY";
inline constexpr std::string_view kUnknownOrder = "-2011 UNKNOWN_ORDER";
inline constexpr std::string_view kOrderMissing = "-2013 ORDER_DOES_NOT_EXIST";
inline constexpr std::string_view kQuantityBelowFilled = "-4028 QUANTITY_BELOW_FILLED";
} // namespace reject

struct SimStats {
  std::uint64_t commands = 0;
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  std::uint64_t taker_fills = 0;
  std::uint64_t rejected = 0;
  std::uint64_t expired = 0;
  std::uint64_t canceled = 0;
};

// A resting order, for snapshots (VenueClient) and tests.
struct SimOrder {
  model::ClientOrderId client_order_id;
  model::VenueOrderId venue_order_id;
  std::uint16_t strategy = 0;
  std::uint32_t slot = 0;
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::Buy;
  model::OrderType type = model::OrderType::Limit;
  model::TimeInForce time_in_force = model::TimeInForce::Gtc;
  bool post_only = false;
  bool reduce_only = false;
  model::Price price;
  model::Quantity quantity;
  std::uint64_t filled_raw = 0;
  std::optional<core::UnixNanos> expire_time;
  std::uint64_t ahead_raw = 0; // volume ahead in the queue (QueuePosition)
  bool active = false;

  [[nodiscard]] std::uint64_t leaves_raw() const noexcept { return quantity.raw() - filled_raw; }
};

class SimulatedExchange {
public:
  explicit SimulatedExchange(const SimConfig& c)
      : config_{c}, markets_{c.instruments}, orders_{c.orders}, strategy_ids_{c.strategies},
        levels_{c.walk_levels}, out_{c.events},
        portfolio_{portfolio::PortfolioConfig{c.instruments, 1, 16, c.margin}},
        rng_{c.seed ^ kSalt} {
    for (std::uint32_t i = 0; i < c.instruments; ++i) {
      static_cast<void>(markets_.push_back(Market{}));
    }
    for (std::uint32_t i = 0; i < c.strategies; ++i) {
      static_cast<void>(strategy_ids_.push_back(model::StrategyId{}));
    }
    for (std::uint32_t i = 0; i < c.walk_levels; ++i) {
      static_cast<void>(levels_.push_back(data::BookLevel{}));
    }
  }

  void set_strategy_id(std::uint16_t s, const model::StrategyId& id) noexcept {
    if (s < strategy_ids_.size()) {
      strategy_ids_[s] = id;
    }
  }

  // The events the last call produced, at venue time (ts_event = ts_init = venue time).
  [[nodiscard]] std::span<const model::OrderEvent> events() const noexcept { return out_.span(); }
  void clear_events() noexcept { out_.clear(); }

  [[nodiscard]] const SimStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const portfolio::Portfolio& account() const noexcept { return portfolio_; }

  [[nodiscard]] core::Status set_account(const model::AccountState& state) noexcept {
    return portfolio_.set_account(state);
  }

  // Resting orders, oldest first; writes up to out.size() and returns how many there are.
  [[nodiscard]] std::size_t open_orders(std::span<SimOrder> out) const noexcept {
    std::size_t n = 0;
    for (const SimOrder& o : orders_.span()) {
      if (!o.active) {
        continue;
      }
      if (n < out.size()) {
        out[n] = o;
      }
      ++n;
    }
    return n;
  }

  // ---- market data (venue time) ---------------------------------------------------------------

  // Not noexcept: instrument definitions go through std::visit.
  [[nodiscard]] core::Status on_data(const model::Event& event, core::UnixNanos now) {
    now_ = now;
    core::Status s = std::visit([this](const auto& e) { return this->data(e); }, event);
    if (core::ok(s)) {
      s = expire_due();
    }
    return s;
  }

  // ---- commands (venue time) ------------------------------------------------------------------

  [[nodiscard]] core::Status on_command(const model::Output& command, core::UnixNanos now) {
    now_ = now;
    core::Status s = core::Status::Ok;
    if (const auto* submit = std::get_if<model::SubmitOrder>(&command)) {
      ++stats_.commands;
      s = on_submit(*submit);
    } else if (const auto* modify = std::get_if<model::ModifyOrder>(&command)) {
      ++stats_.commands;
      s = on_modify(*modify);
    } else if (const auto* cancel = std::get_if<model::CancelOrder>(&command)) {
      ++stats_.commands;
      s = on_cancel(*cancel);
    } else if (const auto* all = std::get_if<model::CancelAllOrders>(&command)) {
      ++stats_.commands;
      s = on_cancel_all(*all);
    }
    if (core::ok(s)) {
      s = expire_due();
    }
    return s;
  }

private:
  static constexpr std::uint64_t kSalt = 0x3C6EF372FE94F82BULL; // venue event ids

  struct Market {
    bool defined = false;
    model::InstrumentId id;
    model::Instrument instrument;
    std::optional<model::Price> bid;
    std::optional<model::Price> ask;
    std::uint64_t bid_size = 0;
    std::uint64_t ask_size = 0;
    std::optional<data::OrderBook> book; // L2, from deltas
  };

  // ---- data handlers --------------------------------------------------------------------------

  template <typename T> core::Status data(const T& e) {
    if constexpr (std::is_same_v<T, model::QuoteTick>) {
      return on_quote(e);
    } else if constexpr (std::is_same_v<T, model::TradeTick>) {
      return on_trade(e);
    } else if constexpr (std::is_same_v<T, model::OrderBookDeltas>) {
      return on_deltas(e);
    } else if constexpr (std::is_same_v<T, model::MarkPriceUpdate>) {
      if (const std::optional<std::uint32_t> slot = slot_of(e.instrument_id)) {
        portfolio_.set_mark(*slot, e.value);
      }
      return core::Status::Ok;
    } else if constexpr (std::is_same_v<T, model::FundingRateUpdate>) {
      if (const std::optional<std::uint32_t> slot = slot_of(e.instrument_id)) {
        std::span<const portfolio::FundingShare> shares;
        return portfolio_.on_funding(markets_[*slot].instrument, *slot, e, shares);
      }
      return core::Status::Ok;
    } else if constexpr (std::is_same_v<T, model::CurrencyPair> ||
                         std::is_same_v<T, model::CryptoPerpetual> ||
                         std::is_same_v<T, model::CryptoFuture>) {
      return define(model::Instrument{e});
    } else if constexpr (std::is_same_v<T, model::AccountState>) {
      return portfolio_.set_account(e);
    } else {
      return core::Status::Ok;
    }
  }

  core::Status define(const model::Instrument& instrument) {
    const model::InstrumentCommon& c = model::common(instrument);
    std::optional<std::uint32_t> slot = slot_of(c.id);
    if (!slot) {
      for (std::uint32_t i = 0; i < markets_.size(); ++i) {
        if (!markets_[i].defined) {
          slot = i;
          break;
        }
      }
    }
    if (!slot) {
      return core::Status::CapacityExceeded;
    }
    Market& m = markets_[*slot];
    m.defined = true;
    m.id = c.id;
    m.instrument = instrument;
    return core::Status::Ok;
  }

  core::Status on_quote(const model::QuoteTick& q) {
    const std::optional<std::uint32_t> slot = slot_of(q.instrument_id);
    if (!slot) {
      return core::Status::Ok;
    }
    Market& m = markets_[*slot];
    const std::optional<model::Price> old_bid = m.bid;
    const std::optional<model::Price> old_ask = m.ask;
    const std::uint64_t old_bid_size = m.bid_size;
    const std::uint64_t old_ask_size = m.ask_size;
    m.bid = q.bid_price;
    m.ask = q.ask_price;
    m.bid_size = q.bid_size.raw();
    m.ask_size = q.ask_size.raw();
    if (m.book) {
      return core::Status::Ok; // the L2 book drives matching
    }
    if (config_.fill_model == FillModel::QueuePosition) {
      const Top before{old_bid, old_ask, old_bid_size, old_ask_size};
      for (SimOrder& o : orders_.span()) {
        if (o.active && o.slot == *slot) {
          follow_top(o, m, before);
        }
      }
    }
    return match_resting(*slot);
  }

  struct Top {
    std::optional<model::Price> bid;
    std::optional<model::Price> ask;
    std::uint64_t bid_size = 0;
    std::uint64_t ask_size = 0;
  };

  // QueuePosition on quotes: the order's level shrank (in proportion), or the market left it.
  static void follow_top(SimOrder& o, const Market& m, const Top& before) noexcept {
    const bool buy = o.side == model::OrderSide::Buy;
    const std::optional<model::Price>& same = buy ? m.bid : m.ask;
    const std::optional<model::Price>& was = buy ? before.bid : before.ask;
    if (same && *same == o.price && was && *was == o.price) {
      shrink_queue(o, buy ? before.bid_size : before.ask_size, buy ? m.bid_size : m.ask_size);
      return;
    }
    const bool left = !same || (buy ? *same < o.price : *same > o.price);
    if (left) {
      o.ahead_raw = 0; // nothing is ahead of the order at its level any more
    }
  }

  core::Status on_deltas(const model::OrderBookDeltas& d) {
    const std::optional<std::uint32_t> slot = slot_of(d.instrument_id);
    if (!slot || d.deltas.empty()) {
      return core::Status::Ok;
    }
    Market& m = markets_[*slot];
    if (!m.book) {
      data::BookConfig bc;
      bc.type = model::BookType::L2_MBP;
      bc.tick = model::common(m.instrument).price_increment;
      bc.size_precision = model::common(m.instrument).size_precision;
      bc.window_levels = config_.book_levels;
      bc.overflow_levels = config_.book_overflow_levels;
      m.book.emplace(bc);
    }
    for (const model::OrderBookDelta& delta : d.deltas) {
      if (config_.fill_model == FillModel::QueuePosition &&
          delta.action != model::BookAction::Clear && delta.order.side) {
        const model::OrderSide side = *delta.order.side;
        const std::uint64_t before = m.book->size_at(side, delta.order.price).raw();
        const core::Status s = m.book->apply(delta);
        if (!core::ok(s)) {
          return s;
        }
        const std::uint64_t after = m.book->size_at(side, delta.order.price).raw();
        for (SimOrder& o : orders_.span()) {
          if (o.active && o.slot == *slot && o.side == side && o.price == delta.order.price) {
            shrink_queue(o, before, after);
          }
        }
        continue;
      }
      const core::Status s = m.book->apply(delta);
      if (!core::ok(s)) {
        return s;
      }
    }
    return match_resting(*slot);
  }

  core::Status on_trade(const model::TradeTick& t) {
    const std::optional<std::uint32_t> slot = slot_of(t.instrument_id);
    if (!slot) {
      return core::Status::Ok;
    }
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      SimOrder& o = orders_[i];
      if (!o.active || o.slot != *slot) {
        continue;
      }
      const std::uint64_t volume = trade_volume(o, t);
      if (volume > 0) {
        const core::Status s = fill(i, volume < o.leaves_raw() ? volume : o.leaves_raw(), o.price,
                                    model::LiquiditySide::Maker);
        if (!core::ok(s)) {
          return s;
        }
      }
    }
    return core::Status::Ok;
  }

  // What a trade gives a resting order: all of it when it prints through the order's price, and
  // at the price (QueuePosition) what is left after the queue ahead.
  [[nodiscard]] std::uint64_t trade_volume(SimOrder& o, const model::TradeTick& t) const noexcept {
    const bool buy = o.side == model::OrderSide::Buy;
    // A resting buy is hit by sellers, a resting sell lifted by buyers.
    const bool hits = buy ? t.aggressor_side != model::AggressorSide::Buy
                          : t.aggressor_side != model::AggressorSide::Sell;
    if (!hits) {
      return 0;
    }
    if (buy ? t.price < o.price : t.price > o.price) {
      return t.size.raw();
    }
    if (!(t.price == o.price) || config_.fill_model != FillModel::QueuePosition) {
      return 0;
    }
    const std::uint64_t size = t.size.raw();
    if (size <= o.ahead_raw) {
      o.ahead_raw -= size;
      return 0;
    }
    const std::uint64_t volume = size - o.ahead_raw;
    o.ahead_raw = 0;
    return volume;
  }

  // Volume ahead shrinks with the level: cancellations are spread over the queue in proportion,
  // rounded down to the order's lot so that every fill quantity stays on the lot grid.
  static void shrink_queue(SimOrder& o, std::uint64_t before, std::uint64_t after) noexcept {
    if (after >= before || before == 0) {
      return; // growth joins behind the order
    }
    const std::uint64_t lot = core::kPow10[model::kFixedPrecision - o.quantity.precision()];
    const auto shrunk =
        static_cast<std::uint64_t>(static_cast<core::u128>(o.ahead_raw) * after / before);
    o.ahead_raw = shrunk - shrunk % lot;
  }

  // ---- the opposite side ----------------------------------------------------------------------

  // Levels an order on `side` would take, best first (L2 when present, else the top of book).
  [[nodiscard]] std::size_t opposite(std::uint32_t slot, model::OrderSide side) noexcept {
    Market& m = markets_[slot];
    if (m.book) {
      const std::span<data::BookLevel> out = levels_.span();
      return side == model::OrderSide::Buy ? m.book->asks(out) : m.book->bids(out);
    }
    const std::optional<model::Price>& best = side == model::OrderSide::Buy ? m.ask : m.bid;
    if (!best || levels_.empty()) {
      return 0;
    }
    const std::uint64_t size = side == model::OrderSide::Buy ? m.ask_size : m.bid_size;
    levels_[0].price = *best;
    static_cast<void>(model::Quantity::from_raw(size, model::common(m.instrument).size_precision,
                                                levels_[0].size));
    return 1;
  }

  [[nodiscard]] static bool within(model::OrderSide side, model::Price level,
                                   const std::optional<model::Price>& limit) noexcept {
    if (!limit) {
      return true; // market order
    }
    return side == model::OrderSide::Buy ? level <= *limit : level >= *limit;
  }

  // Volume available to `side` up to `limit` among the opposite levels.
  [[nodiscard]] std::uint64_t available(std::uint32_t slot, model::OrderSide side,
                                        const std::optional<model::Price>& limit) noexcept {
    const std::size_t n = opposite(slot, side);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < n && within(side, levels_[i].price, limit); ++i) {
      total += levels_[i].size.raw();
    }
    return total;
  }

  // Takes up to `want` from the opposite levels; returns what was filled.
  [[nodiscard]] core::Status take(std::size_t index, std::uint64_t want, std::uint64_t& taken) {
    taken = 0;
    SimOrder& o = orders_[index];
    const std::optional<model::Price> limit =
        o.type == model::OrderType::Market ? std::nullopt : std::optional{o.price};
    const std::size_t n = opposite(o.slot, o.side);
    for (std::size_t i = 0; i < n && taken < want; ++i) {
      const data::BookLevel level = levels_[i];
      if (!within(o.side, level.price, limit)) {
        break;
      }
      const std::uint64_t q = level.size.raw() < want - taken ? level.size.raw() : want - taken;
      if (q == 0) {
        continue;
      }
      const core::Status s = fill(index, q, level.price, model::LiquiditySide::Taker);
      if (!core::ok(s)) {
        return s;
      }
      taken += q;
    }
    return core::Status::Ok;
  }

  // Resting orders the opposite best has reached fill at their own price.
  core::Status match_resting(std::uint32_t slot) {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      SimOrder& o = orders_[i];
      if (!o.active || o.slot != slot) {
        continue;
      }
      const std::size_t n = opposite(slot, o.side);
      if (n == 0 || !within(o.side, levels_[0].price, o.price)) {
        continue;
      }
      std::uint64_t volume = 0;
      for (std::size_t k = 0; k < n && within(o.side, levels_[k].price, o.price); ++k) {
        volume += levels_[k].size.raw();
      }
      const std::uint64_t q = volume < o.leaves_raw() ? volume : o.leaves_raw();
      if (q > 0) {
        const core::Status s = fill(i, q, o.price, model::LiquiditySide::Maker);
        if (!core::ok(s)) {
          return s;
        }
      }
    }
    return core::Status::Ok;
  }

  // ---- commands ------------------------------------------------------------------------------

  core::Status on_submit(const model::SubmitOrder& c) {
    const std::optional<std::uint32_t> slot = slot_of(c.instrument_id);
    if (!slot) {
      return reject_submit(c, reject::kUnknownSymbol, false);
    }
    if (c.reduce_only && !reduces(*slot, c.order_side, c.quantity.raw())) {
      return reject_submit(c, reject::kReduceOnly, false);
    }
    const std::optional<model::Price> limit =
        c.order_type == model::OrderType::Market ? std::nullopt : c.price;
    const std::uint64_t liquidity = available(*slot, c.order_side, limit);
    const bool marketable = liquidity > 0;
    if (c.order_type == model::OrderType::Market && !marketable) {
      return reject_submit(c, reject::kNoMarket, false);
    }
    if (c.post_only && marketable) {
      return reject_submit(c, reject::kPostOnly, true);
    }
    if (c.time_in_force == model::TimeInForce::Fok && liquidity < c.quantity.raw()) {
      return emit_expired(c.strategy_index, c.instrument_id, c.client_order_id, std::nullopt);
    }
    std::size_t index = 0;
    if (!add_order(c, *slot, index)) {
      return reject_submit(c, reject::kCapacity, false);
    }
    return place(index, marketable);
  }

  // An accepted order: self-trade prevention, the taker part, then rest or expire.
  core::Status place(std::size_t index, bool marketable) {
    SimOrder& o = orders_[index];
    core::Status s = emit_accepted(o);
    if (!core::ok(s)) {
      return s;
    }
    if (marketable && config_.stp != StpMode::None && self_cross(index)) {
      const bool expire_taker =
          config_.stp == StpMode::ExpireTaker || config_.stp == StpMode::ExpireBoth;
      if (config_.stp == StpMode::ExpireMaker || config_.stp == StpMode::ExpireBoth) {
        s = expire_crossed_makers(index);
        if (!core::ok(s)) {
          return s;
        }
      }
      if (expire_taker) {
        return expire(index);
      }
    }
    if (marketable) {
      std::uint64_t taken = 0;
      s = take(index, o.leaves_raw(), taken);
      if (!core::ok(s)) {
        return s;
      }
    }
    if (!o.active) {
      return core::Status::Ok; // filled
    }
    const bool rests =
        o.type != model::OrderType::Market &&
        (o.time_in_force == model::TimeInForce::Gtc || o.time_in_force == model::TimeInForce::Gtd);
    if (!rests) {
      return expire(index);
    }
    o.ahead_raw = queue_ahead(o);
    return core::Status::Ok;
  }

  core::Status on_modify(const model::ModifyOrder& c) {
    const std::optional<std::size_t> index = find(c.client_order_id);
    if (!index) {
      return emit_modify_rejected(c, reject::kOrderMissing);
    }
    SimOrder& o = orders_[*index];
    if (c.quantity.raw() <= o.filled_raw || c.quantity.precision() != o.quantity.precision()) {
      return emit_modify_rejected(c, reject::kQuantityBelowFilled);
    }
    const std::optional<model::Price> limit{c.price};
    const bool marketable = available(o.slot, o.side, limit) > 0;
    if (o.post_only && marketable) {
      return emit_modify_rejected(c, reject::kPostOnly);
    }
    const bool keeps_priority = c.price == o.price && c.quantity.raw() <= o.quantity.raw();
    o.price = c.price;
    o.quantity = c.quantity;
    core::Status s = emit_updated(o);
    if (!core::ok(s)) {
      return s;
    }
    if (marketable) {
      std::uint64_t taken = 0;
      s = take(*index, o.leaves_raw(), taken);
      if (!core::ok(s)) {
        return s;
      }
    }
    if (o.active && !keeps_priority) {
      o.ahead_raw = queue_ahead(o);
    }
    return core::Status::Ok;
  }

  core::Status on_cancel(const model::CancelOrder& c) {
    const std::optional<std::size_t> index = find(c.client_order_id);
    if (!index) {
      model::OrderCancelRejected e;
      e.header = header(c.strategy_index, c.instrument_id, c.client_order_id);
      static_cast<void>(model::ReasonText::from(reject::kUnknownOrder, e.reason));
      e.venue_order_id = c.venue_order_id;
      e.account_id = config_.account_id;
      return emit(model::OrderEvent{e});
    }
    return cancel(*index);
  }

  core::Status on_cancel_all(const model::CancelAllOrders& c) {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      if (orders_[i].active && orders_[i].instrument_id == c.instrument_id) {
        const core::Status s = cancel(i);
        if (!core::ok(s)) {
          return s;
        }
      }
    }
    return core::Status::Ok;
  }

  core::Status expire_due() {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      const SimOrder& o = orders_[i];
      if (o.active && o.expire_time && *o.expire_time <= now_) {
        const core::Status s = expire(i);
        if (!core::ok(s)) {
          return s;
        }
      }
    }
    return core::Status::Ok;
  }

  // ---- order book of our orders ---------------------------------------------------------------

  [[nodiscard]] bool add_order(const model::SubmitOrder& c, std::uint32_t slot,
                               std::size_t& index) {
    std::size_t free = orders_.size();
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      if (!orders_[i].active) {
        free = i;
        break;
      }
    }
    if (free == orders_.size() && !core::ok(orders_.push_back(SimOrder{}))) {
      return false;
    }
    SimOrder& o = orders_[free];
    o = SimOrder{};
    o.client_order_id = c.client_order_id;
    o.venue_order_id = next_venue_order_id();
    o.strategy = c.strategy_index;
    o.slot = slot;
    o.instrument_id = c.instrument_id;
    o.side = c.order_side;
    o.type = c.order_type;
    o.time_in_force = c.time_in_force;
    o.post_only = c.post_only;
    o.reduce_only = c.reduce_only;
    o.price = c.price.value_or(model::Price{});
    o.quantity = c.quantity;
    o.expire_time = c.time_in_force == model::TimeInForce::Gtd ? c.expire_time : std::nullopt;
    o.active = true;
    index = free;
    return true;
  }

  [[nodiscard]] std::optional<std::size_t> find(const model::ClientOrderId& id) const noexcept {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      if (orders_[i].active && orders_[i].client_order_id == id) {
        return i;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::uint32_t> slot_of(const model::InstrumentId& id) const noexcept {
    for (std::uint32_t i = 0; i < markets_.size(); ++i) {
      if (markets_[i].defined && markets_[i].id == id) {
        return i;
      }
    }
    return std::nullopt;
  }

  // Volume on the order's own side at its price (the queue it joins).
  [[nodiscard]] std::uint64_t queue_ahead(const SimOrder& o) const noexcept {
    const Market& m = markets_[o.slot];
    if (m.book) {
      return m.book->size_at(o.side, o.price).raw();
    }
    const bool buy = o.side == model::OrderSide::Buy;
    const std::optional<model::Price>& best = buy ? m.bid : m.ask;
    if (!best || !(*best == o.price)) {
      return 0;
    }
    return buy ? m.bid_size : m.ask_size;
  }

  // One-way mode: the order must reduce the venue position, and by no more than it.
  [[nodiscard]] bool reduces(std::uint32_t slot, model::OrderSide side,
                             std::uint64_t quantity) const noexcept {
    const std::int64_t position = portfolio_.venue(slot).signed_raw();
    const bool opposite = position != 0 && (position > 0) != (side == model::OrderSide::Buy);
    return opposite && quantity <= core::magnitude(position);
  }

  [[nodiscard]] bool self_cross(std::size_t taker) const noexcept {
    const SimOrder& t = orders_[taker];
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      const SimOrder& o = orders_[i];
      if (i != taker && o.active && o.slot == t.slot && o.side != t.side &&
          (t.type == model::OrderType::Market || within(t.side, o.price, t.price))) {
        return true;
      }
    }
    return false;
  }

  core::Status expire_crossed_makers(std::size_t taker) {
    const SimOrder t = orders_[taker];
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      const SimOrder& o = orders_[i];
      if (i != taker && o.active && o.slot == t.slot && o.side != t.side &&
          (t.type == model::OrderType::Market || within(t.side, o.price, t.price))) {
        const core::Status s = expire(i);
        if (!core::ok(s)) {
          return s;
        }
      }
    }
    return core::Status::Ok;
  }

  // ---- fills and events -----------------------------------------------------------------------

  core::Status fill(std::size_t index, std::uint64_t qty_raw, model::Price price,
                    model::LiquiditySide liquidity) {
    SimOrder& o = orders_[index];
    const Market& m = markets_[o.slot];
    const model::InstrumentCommon& c = model::common(m.instrument);
    model::Quantity qty;
    core::Status s = model::Quantity::from_raw(qty_raw, o.quantity.precision(), qty);
    if (!core::ok(s)) {
      return s;
    }
    model::Money commission;
    s = config_.fees.commission(c, liquidity, qty, price, commission);
    if (!core::ok(s)) {
      return s;
    }
    o.filled_raw += qty_raw;
    if (o.leaves_raw() == 0) {
      o.active = false;
    }
    portfolio::FillOutcome outcome;
    s = portfolio_.on_fill(m.instrument, o.slot, 0, o.side, qty, price, commission,
                           o.client_order_id, now_, outcome);
    if (!core::ok(s)) {
      return s;
    }
    model::OrderFilled e;
    e.header = header(o.strategy, o.instrument_id, o.client_order_id);
    e.venue_order_id = o.venue_order_id;
    e.account_id = config_.account_id;
    e.trade_id = next_trade_id();
    e.order_side = o.side;
    e.order_type = o.type;
    e.last_qty = qty;
    e.last_px = price;
    e.currency = c.settlement_currency;
    e.liquidity_side = liquidity;
    e.commission = commission;
    ++stats_.fills;
    (liquidity == model::LiquiditySide::Maker ? stats_.maker_fills : stats_.taker_fills) += 1;
    return emit(model::OrderEvent{e});
  }

  core::Status cancel(std::size_t index) {
    SimOrder& o = orders_[index];
    o.active = false;
    model::OrderCanceled e;
    e.header = header(o.strategy, o.instrument_id, o.client_order_id);
    e.venue_order_id = o.venue_order_id;
    e.account_id = config_.account_id;
    ++stats_.canceled;
    return emit(model::OrderEvent{e});
  }

  core::Status expire(std::size_t index) {
    SimOrder& o = orders_[index];
    o.active = false;
    return emit_expired(o.strategy, o.instrument_id, o.client_order_id, o.venue_order_id);
  }

  core::Status emit_expired(std::uint16_t strategy, const model::InstrumentId& iid,
                            const model::ClientOrderId& cid,
                            const std::optional<model::VenueOrderId>& vid) {
    model::OrderExpired e;
    e.header = header(strategy, iid, cid);
    e.venue_order_id = vid;
    e.account_id = config_.account_id;
    ++stats_.expired;
    return emit(model::OrderEvent{e});
  }

  core::Status emit_accepted(const SimOrder& o) {
    model::OrderAccepted e;
    e.header = header(o.strategy, o.instrument_id, o.client_order_id);
    e.venue_order_id = o.venue_order_id;
    e.account_id = config_.account_id;
    return emit(model::OrderEvent{e});
  }

  core::Status emit_updated(const SimOrder& o) {
    model::OrderUpdated e;
    e.header = header(o.strategy, o.instrument_id, o.client_order_id);
    e.venue_order_id = o.venue_order_id;
    e.account_id = config_.account_id;
    e.quantity = o.quantity;
    e.price = o.price;
    return emit(model::OrderEvent{e});
  }

  core::Status emit_modify_rejected(const model::ModifyOrder& c, std::string_view reason) {
    model::OrderModifyRejected e;
    e.header = header(c.strategy_index, c.instrument_id, c.client_order_id);
    static_cast<void>(model::ReasonText::from(reason, e.reason));
    e.venue_order_id = c.venue_order_id;
    e.account_id = config_.account_id;
    return emit(model::OrderEvent{e});
  }

  core::Status reject_submit(const model::SubmitOrder& c, std::string_view reason,
                             bool due_post_only) {
    model::OrderRejected e;
    e.header = header(c.strategy_index, c.instrument_id, c.client_order_id);
    e.account_id = config_.account_id;
    static_cast<void>(model::ReasonText::from(reason, e.reason));
    e.due_post_only = due_post_only;
    ++stats_.rejected;
    return emit(model::OrderEvent{e});
  }

  [[nodiscard]] model::OrderEventHeader header(std::uint16_t strategy,
                                               const model::InstrumentId& iid,
                                               const model::ClientOrderId& cid) noexcept {
    model::OrderEventHeader h;
    h.trader_id = config_.trader_id;
    if (strategy < strategy_ids_.size()) {
      h.strategy_id = strategy_ids_[strategy];
    }
    h.instrument_id = iid;
    h.client_order_id = cid;
    h.event_id = model::Uuid4::derive(rng_, ++event_serial_, 0);
    h.ts_event = now_;
    h.ts_init = now_;
    return h;
  }

  core::Status emit(const model::OrderEvent& e) { return out_.push_back(e); }

  [[nodiscard]] model::VenueOrderId next_venue_order_id() noexcept {
    return numbered<model::VenueOrderId>('V', ++venue_serial_);
  }
  [[nodiscard]] model::TradeId next_trade_id() noexcept {
    return numbered<model::TradeId>('T', ++trade_serial_);
  }
  template <typename Id> [[nodiscard]] static Id numbered(char prefix, std::uint64_t n) noexcept {
    std::array<char, 24> text{};
    std::size_t len = 0;
    text[len++] = prefix;
    std::array<char, 20> digits{};
    std::size_t d = 0;
    do {
      digits[d++] = static_cast<char>('0' + n % 10);
      n /= 10;
    } while (n > 0);
    while (d > 0) {
      text[len++] = digits[--d];
    }
    Id id;
    static_cast<void>(Id::from(std::string_view{text.data(), len}, id));
    return id;
  }

  SimConfig config_;
  core::FixedVector<Market> markets_;
  core::FixedVector<SimOrder> orders_;
  core::FixedVector<model::StrategyId> strategy_ids_;
  core::FixedVector<data::BookLevel> levels_;
  core::FixedVector<model::OrderEvent> out_;
  portfolio::Portfolio portfolio_;
  core::CounterRng rng_;
  core::UnixNanos now_;
  SimStats stats_;
  std::uint64_t event_serial_ = 0;
  std::uint64_t venue_serial_ = 0;
  std::uint64_t trade_serial_ = 0;
};

} // namespace jarvis::backtest
