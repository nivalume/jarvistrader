#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"

namespace jarvis::model {

using core::UnixNanos;

// Market data types with nautilus field names and meanings (docs/architecture.md section 6.4).

struct QuoteTick {
  InstrumentId instrument_id;
  Price bid_price;
  Price ask_price;
  Quantity bid_size;
  Quantity ask_size;
  UnixNanos ts_event;
  UnixNanos ts_init;

  // Bid and ask prices share a precision, as do the sizes (nautilus QuoteTick::new_checked).
  [[nodiscard]] static constexpr core::Status create(const InstrumentId& id, Price bid, Price ask,
                                                     Quantity bid_size, Quantity ask_size,
                                                     UnixNanos ts_event, UnixNanos ts_init,
                                                     QuoteTick& out) noexcept {
    if (bid.precision() != ask.precision() || bid_size.precision() != ask_size.precision()) {
      return core::Status::InvalidArgument;
    }
    out = QuoteTick{id, bid, ask, bid_size, ask_size, ts_event, ts_init};
    return core::Status::Ok;
  }
};

struct TradeTick {
  InstrumentId instrument_id;
  Price price;
  Quantity size;
  AggressorSide aggressor_side = AggressorSide::NoAggressor;
  TradeId trade_id;
  UnixNanos ts_event;
  UnixNanos ts_init;

  // A trade has a positive size (nautilus TradeTick::new_checked).
  [[nodiscard]] static constexpr core::Status create(const InstrumentId& id, Price price,
                                                     Quantity size, AggressorSide side,
                                                     const TradeId& trade_id, UnixNanos ts_event,
                                                     UnixNanos ts_init, TradeTick& out) noexcept {
    if (size.is_zero() || trade_id.empty()) {
      return core::Status::InvalidArgument;
    }
    out = TradeTick{id, price, size, side, trade_id, ts_event, ts_init};
    return core::Status::Ok;
  }
};

// RecordFlag bits carried in OrderBookDelta::flags.
[[nodiscard]] constexpr std::uint8_t flag_bit(RecordFlag flag) noexcept {
  return static_cast<std::uint8_t>(flag);
}
[[nodiscard]] constexpr bool has_flag(std::uint8_t flags, RecordFlag flag) noexcept {
  return (flags & flag_bit(flag)) != 0;
}

struct BookOrder {
  std::optional<OrderSide> side;
  Price price;
  Quantity size;
  std::uint64_t order_id = 0;
};

struct OrderBookDelta {
  InstrumentId instrument_id;
  BookAction action = BookAction::Add;
  BookOrder order;
  std::uint8_t flags = 0;
  std::uint64_t sequence = 0;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

// A batch of deltas for one instrument. The deltas are borrowed: whoever produced the batch owns
// the storage for as long as the batch is being processed.
struct OrderBookDeltas {
  InstrumentId instrument_id;
  std::span<const OrderBookDelta> deltas;
  std::uint8_t flags = 0;
  std::uint64_t sequence = 0;
  UnixNanos ts_event;
  UnixNanos ts_init;

  // flags and sequence come from the last delta, like nautilus.
  [[nodiscard]] static constexpr core::Status create(std::span<const OrderBookDelta> deltas,
                                                     OrderBookDeltas& out) noexcept {
    if (deltas.empty()) {
      return core::Status::InvalidArgument;
    }
    for (const OrderBookDelta& d : deltas) {
      if (!(d.instrument_id == deltas.front().instrument_id)) {
        return core::Status::InvalidArgument;
      }
    }
    const OrderBookDelta& last = deltas.back();
    out = OrderBookDeltas{last.instrument_id, deltas,        last.flags,
                          last.sequence,      last.ts_event, last.ts_init};
    return core::Status::Ok;
  }
};

inline constexpr std::size_t kDepthLevels = 10;

// Aggregated depth of up to ten levels per side (nautilus OrderBookDepth with its inline
// capacity). Levels beyond `bid_levels` / `ask_levels` are unused.
struct OrderBookDepth {
  InstrumentId instrument_id;
  std::array<BookOrder, kDepthLevels> bids{};
  std::array<BookOrder, kDepthLevels> asks{};
  std::array<std::uint32_t, kDepthLevels> bid_counts{};
  std::array<std::uint32_t, kDepthLevels> ask_counts{};
  std::uint8_t bid_levels = 0;
  std::uint8_t ask_levels = 0;
  std::uint8_t flags = 0;
  std::uint64_t sequence = 0;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

using ShortText = core::FixedString<32>;

struct InstrumentStatus {
  InstrumentId instrument_id;
  MarketStatusAction action = MarketStatusAction::None;
  UnixNanos ts_event;
  UnixNanos ts_init;
  std::optional<ShortText> reason;
  std::optional<ShortText> trading_event;
  std::optional<bool> is_trading;
  std::optional<bool> is_quoting;
  std::optional<bool> is_short_sell_restricted;
};

struct MarkPriceUpdate {
  InstrumentId instrument_id;
  Price value;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

struct IndexPriceUpdate {
  InstrumentId instrument_id;
  Price value;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

struct FundingRateUpdate {
  InstrumentId instrument_id;
  Decimal rate;
  std::optional<std::uint16_t> interval; // minutes
  std::optional<UnixNanos> next_funding_ns;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

struct InstrumentClose {
  InstrumentId instrument_id;
  Price close_price;
  InstrumentCloseType close_type = InstrumentCloseType::EndOfSession;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

// jarvis extension (not a nautilus type): a liquidation order published by the venue, from the
// Binance forceOrder stream. Stored as custom data when exported to a nautilus catalog.
struct LiquidationOrder {
  InstrumentId instrument_id;
  OrderSide side = OrderSide::Buy;
  Price price;
  Quantity quantity;
  Price average_price;
  Quantity filled_quantity;
  UnixNanos ts_event;
  UnixNanos ts_init;
};

} // namespace jarvis::model
