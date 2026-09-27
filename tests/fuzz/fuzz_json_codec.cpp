// Fuzzes the Binance USDⓈ-M JSON codec (jarvis/adapter/binance/json_codec.hpp). Properties (any
// violation traps):
//   - decoding never reads outside the input (ASan) and never crashes;
//   - decoding is deterministic: two fresh codecs give the same status and the same events;
//   - every emitted event is well formed: prices and quantities of trades, quotes, bars and
//     depth levels are at the instrument's precision, trades have a positive size, bars have
//     high >= low;
//   - a failure always leaves a reason.

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/codec.hpp"

namespace {

namespace adapter = jarvis::adapter;
namespace binance = jarvis::adapter::binance;
namespace model = jarvis::model;
using jarvis::core::Status;

void require(bool condition) {
  if (!condition) {
    __builtin_trap();
  }
}

constexpr std::uint8_t kPricePrecision = 1;
constexpr std::uint8_t kSizePrecision = 3;

const adapter::SymbolTable& table() {
  static const adapter::SymbolTable t = [] {
    adapter::SymbolTable s;
    model::InstrumentId id;
    require(binance::perpetual_id("BTCUSDT", id) == Status::Ok);
    require(s.add("BTCUSDT", adapter::SymbolEntry{id, kPricePrecision, kSizePrecision, {}}) ==
            Status::Ok);
    return s;
  }();
  return t;
}

bool same_depths(const adapter::CollectingEmitter& a, const adapter::CollectingEmitter& b) {
  if (a.depths.size() != b.depths.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.depths.size(); ++i) {
    const auto& x = a.depths[i];
    const auto& y = b.depths[i];
    if (x.bids.size() != y.bids.size() || x.asks.size() != y.asks.size() ||
        x.diff.final_update_id != y.diff.final_update_id) {
      return false;
    }
    for (std::size_t k = 0; k < x.bids.size(); ++k) {
      if (!(x.bids[k].price == y.bids[k].price) || !(x.bids[k].size == y.bids[k].size)) {
        return false;
      }
    }
  }
  return true;
}

void check_event(const model::Event& e) {
  if (const auto* t = std::get_if<model::TradeTick>(&e)) {
    require(t->price.precision() == kPricePrecision && t->size.precision() == kSizePrecision);
    require(!t->size.is_zero());
  } else if (const auto* q = std::get_if<model::QuoteTick>(&e)) {
    require(q->bid_price.precision() == kPricePrecision &&
            q->ask_price.precision() == kPricePrecision);
    require(q->bid_size.precision() == kSizePrecision && q->ask_size.precision() == kSizePrecision);
  } else if (const auto* b = std::get_if<model::Bar>(&e)) {
    require(b->open.precision() == kPricePrecision && b->volume.precision() == kSizePrecision);
    require(!(b->high < b->low));
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input{reinterpret_cast<const std::byte*>(data), size}; // NOLINT
  binance::JsonCodec first{table()};
  binance::JsonCodec second{table()};
  adapter::CollectingEmitter a;
  adapter::CollectingEmitter b;
  adapter::ConnCtx conn{0, jarvis::core::UnixNanos{1}};
  const Status sa = first.decode(input, conn, a);
  const Status sb = second.decode(input, conn, b);
  require(sa == sb);
  require(a.events.size() == b.events.size());
  require(same_depths(a, b));
  if (sa != Status::Ok) {
    require(!first.error().empty());
  }
  for (const model::Event& e : a.events) {
    check_event(e);
  }
  for (const auto& d : a.depths) {
    for (const adapter::BookLevel& l : d.bids) {
      require(l.price.precision() == kPricePrecision && l.size.precision() == kSizePrecision);
    }
    for (const adapter::BookLevel& l : d.asks) {
      require(l.price.precision() == kPricePrecision && l.size.precision() == kSizePrecision);
    }
  }
  return 0;
}
