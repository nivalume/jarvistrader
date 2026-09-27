#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"

// JSON codec of the Binance USDⓈ-M market streams (docs/architecture.md sections 13.3 and
// 14.2), on simdjson on-demand. Numbers arrive as strings and are parsed straight to fixed
// point at the instrument's precision; no value passes through floating point.
//
//   aggTrade        -> TradeTick (aggregate id as trade_id; m = buyer is maker -> SELL)
//   bookTicker      -> QuoteTick (ts_event = T; updates with u not above the last are dropped)
//   markPriceUpdate -> MarkPriceUpdate, IndexPriceUpdate, FundingRateUpdate (ts_event = E)
//   kline           -> Bar ...-LAST-EXTERNAL, closed klines only, stamped at the close
//   forceOrder      -> LiquidationOrder
//   depthUpdate     -> DepthDiff for the depth synchronizer
//
// Fields are read in the order Binance sends them, with ordered lookups only: simdjson's
// unordered lookup assumes an already validated object when it wraps around, which malformed
// input could violate (tests/fuzz/fuzz_json_codec.cpp found it). New fields in between are
// skipped; a reordered or missing field fails as ParseError naming the field.
//
// Both the combined-stream envelope {"stream":..,"data":{..}} and bare events are accepted;
// subscription answers ({"result":..,"id":..}) are ignored. Messages for symbols outside the
// table are counted and skipped (the all-symbol liquidation stream sends every symbol).

namespace jarvis::adapter::binance {

struct CodecStats {
  std::uint64_t messages = 0;
  std::uint64_t events = 0;
  std::uint64_t depth_diffs = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t stale_quotes = 0; // bookTicker updates at or below the last update id
  std::uint64_t open_klines = 0;  // klines not yet closed
  std::uint64_t control = 0;      // subscription answers
  std::uint64_t unsupported = 0;
  std::uint64_t errors = 0;
};

class JsonCodec {
public:
  // The table must outlive the codec and not grow after the codec is built.
  explicit JsonCodec(const SymbolTable& symbols);
  ~JsonCodec();
  JsonCodec(const JsonCodec&) = delete;
  JsonCodec& operator=(const JsonCodec&) = delete;
  JsonCodec(JsonCodec&&) noexcept;
  JsonCodec& operator=(JsonCodec&&) noexcept;

  // Ok (possibly with no events); ParseError, PrecisionLoss, OutOfRange or InvalidArgument for
  // a malformed message; UnsupportedMessage for an event type this codec does not know. The
  // reason of the last failure is in error().
  [[nodiscard]] core::Status decode(std::span<const std::byte> frame, ConnCtx& conn,
                                    EventEmitter& out);

  [[nodiscard]] const CodecStats& stats() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

static_assert(Codec<JsonCodec>);

} // namespace jarvis::adapter::binance
