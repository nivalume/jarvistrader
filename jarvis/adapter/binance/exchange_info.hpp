#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"

// Instruments from a USDⓈ-M /fapi/v1/exchangeInfo response (docs/architecture.md section 14.6).
// The mapping is the one python/jarvis/data/binance_instrument.py uses for backtest catalogs,
// so a live node and a backtest see the same instrument:
//
//   id                 {symbol}-PERP.BINANCE
//   price precision    decimals of PRICE_FILTER.tickSize without trailing zeros ("0.10" -> 1)
//   size precision     decimals of LOT_SIZE.stepSize the same way
//   min/max price      PRICE_FILTER minPrice/maxPrice at the price precision
//   min/max quantity   LOT_SIZE minQty/maxQty at the size precision
//   min_notional       MIN_NOTIONAL.notional in the margin asset
//   margins            requiredMarginPercent / 100 and maintMarginPercent / 100
//
// Gate B's venue rules (PRICE_FILTER, LOT_SIZE, MIN_NOTIONAL) read these fields directly.
// Symbols whose status is not TRADING load but are reported as not tradable.

namespace jarvis::adapter::binance {

struct PerpetualDefinition {
  model::CryptoPerpetual instrument;
  bool trading = false; // status == TRADING
};

// BTCUSDT -> BTCUSDT-PERP.BINANCE
[[nodiscard]] core::Status perpetual_id(std::string_view symbol, model::InstrumentId& out);

// Loads the perpetuals named in `symbols` (every perpetual when empty). NotFound when a named
// symbol is missing, InvalidArgument when it is not a perpetual; ParseError for a malformed
// response. `error` says which.
[[nodiscard]] core::Status
parse_exchange_info(std::string_view json, std::span<const std::string> symbols, core::UnixNanos ts,
                    std::vector<PerpetualDefinition>& out, std::string& error);

} // namespace jarvis::adapter::binance
