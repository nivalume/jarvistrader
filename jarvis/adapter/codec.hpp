#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"

// Wire codecs of the venue adapters (docs/architecture.md section 13.3). A codec runs on an IO
// thread and turns one received message into normalized fixed-point events; the kernel never
// sees the wire format. Depth diffs are not events yet: they go to the depth synchronizer
// (section 14.3), which emits OrderBookDeltas once the book is in sync.

namespace jarvis::adapter {

// The connection a message arrived on and when (UTC nanoseconds from the arrival clock; it
// becomes ts_init).
struct ConnCtx {
  std::uint32_t conn_id = 0;
  core::UnixNanos recv_ns;
};

// One price level of a depth diff: the new absolute size, zero removing the level.
struct BookLevel {
  model::Price price;
  model::Quantity size;
};

struct DepthDiff {
  std::uint32_t symbol = 0; // SymbolTable index
  model::InstrumentId instrument_id;
  std::uint64_t first_update_id = 0;      // Binance U
  std::uint64_t final_update_id = 0;      // Binance u
  std::uint64_t prev_final_update_id = 0; // Binance pu (USDⓈ-M only)
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
  std::span<const BookLevel> bids; // valid for the duration of the call
  std::span<const BookLevel> asks;
};

class EventEmitter {
public:
  EventEmitter() = default;
  EventEmitter(const EventEmitter&) = default;
  EventEmitter& operator=(const EventEmitter&) = default;
  EventEmitter(EventEmitter&&) = default;
  EventEmitter& operator=(EventEmitter&&) = default;
  virtual ~EventEmitter() = default;

  [[nodiscard]] virtual core::Status event(const model::Event& e) = 0;
  [[nodiscard]] virtual core::Status depth(const DepthDiff& d) = 0;
};

// Collects into vectors (tests, tools, redecode); depth levels are copied.
class CollectingEmitter final : public EventEmitter {
public:
  struct Depth {
    DepthDiff diff;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
  };

  core::Status event(const model::Event& e) override {
    events.push_back(e);
    return core::Status::Ok;
  }
  core::Status depth(const DepthDiff& d) override {
    Depth copy{d, {d.bids.begin(), d.bids.end()}, {d.asks.begin(), d.asks.end()}};
    copy.diff.bids = copy.bids;
    copy.diff.asks = copy.asks;
    depths.push_back(std::move(copy));
    return core::Status::Ok;
  }
  void clear() {
    events.clear();
    depths.clear();
  }

  std::vector<model::Event> events;
  std::vector<Depth> depths;
};

template <typename C>
concept Codec = requires(C c, std::span<const std::byte> frame, ConnCtx& conn, EventEmitter& out) {
  { c.decode(frame, conn, out) } -> std::same_as<core::Status>;
};

// The instruments an adapter serves, by venue symbol ("BTCUSDT"). Built before the codecs, at
// startup from exchangeInfo; lookups take string views without allocating.
struct SymbolEntry {
  model::InstrumentId id;
  std::uint8_t price_precision = 0;
  std::uint8_t size_precision = 0;
};

class SymbolTable {
public:
  [[nodiscard]] core::Status add(std::string_view venue_symbol, const SymbolEntry& entry);
  [[nodiscard]] core::Status add(const model::InstrumentCommon& instrument) {
    return add(instrument.raw_symbol.view(),
               SymbolEntry{instrument.id, instrument.price_precision, instrument.size_precision});
  }
  [[nodiscard]] std::optional<std::uint32_t> find(std::string_view venue_symbol) const;
  [[nodiscard]] const SymbolEntry& operator[](std::uint32_t index) const { return entries_[index]; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  std::unordered_map<std::string, std::uint32_t, Hash, std::equal_to<>> index_;
  std::vector<SymbolEntry> entries_;
};

// Fixed-point values from venue strings, exactly: the value must lie on the grid of the given
// precision ("84551.10" at precision 1 is fine, "84551.15" is PrecisionLoss). No floating point.
[[nodiscard]] core::Status exact_price(std::string_view text, std::uint8_t precision,
                                       model::Price& out);
[[nodiscard]] core::Status exact_quantity(std::string_view text, std::uint8_t precision,
                                          model::Quantity& out);

} // namespace jarvis::adapter
