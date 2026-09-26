#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"

// Typed subscriptions and delivery cadences (docs/architecture.md sections 7.2 and 7.5).

namespace jarvis::data {

// What a subscription delivers. Each kind has its own row in the subscription matrix.
enum class DataKind : std::uint8_t {
  Trade = 0,        // TradeTick
  Quote = 1,        // QuoteTick
  BookDeltas = 2,   // OrderBookDeltas as received
  Book = 3,         // BookView after the deltas are applied
  Bar = 4,          // Bar (external or aggregated); rows are bar keys, not instruments
  MarkPrice = 5,    // MarkPriceUpdate
  IndexPrice = 6,   // IndexPriceUpdate
  FundingRate = 7,  // FundingRateUpdate
  Status = 8,       // InstrumentStatus
  Close = 9,        // InstrumentClose
  Liquidation = 10, // LiquidationOrder
  Feature = 11,     // FeatureUpdate; rows are feature ids
};
inline constexpr std::size_t kDataKindCount = 12;

// When a subscriber sees updates.
//   Every      each update, as it is processed;
//   Conflated  once per batch, the latest update of the batch (delivered at BatchEnd);
//   Sampled    the first update in each period of `period` nanoseconds, periods aligned to the
//              Unix epoch: at most one delivery per period, never delayed;
//   OnBatch    once per batch, all updates of the batch together (on_batch).
// Batches end with BatchEnd events, which are recorded inputs, so replay merges identically.
struct Cadence {
  enum class Mode : std::uint8_t { Every = 0, Conflated = 1, Sampled = 2, OnBatch = 3 };

  Mode mode = Mode::Every;
  core::DurationNanos period;

  [[nodiscard]] static constexpr Cadence every() noexcept { return Cadence{}; }
  [[nodiscard]] static constexpr Cadence conflated() noexcept { return {Mode::Conflated, {}}; }
  [[nodiscard]] static constexpr Cadence on_batch() noexcept { return {Mode::OnBatch, {}}; }
  [[nodiscard]] static constexpr Cadence sampled_ns(std::uint64_t ns) noexcept {
    return {Mode::Sampled, core::DurationNanos{ns}};
  }
  [[nodiscard]] static constexpr Cadence sampled_ms(std::uint64_t ms) noexcept {
    return sampled_ns(ms * 1'000'000U);
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return mode != Mode::Sampled || period.value() > 0;
  }

  friend constexpr bool operator==(const Cadence&, const Cadence&) noexcept = default;
};

using StrategyIndex = std::uint16_t;

// One subscriber of a matrix cell and its delivery state.
struct Subscriber {
  StrategyIndex strategy = 0;
  Cadence cadence;
  std::uint64_t last_period = UINT64_MAX; // Sampled: period index of the last delivery
  std::uint32_t buffer = UINT32_MAX;      // Conflated or OnBatch: index of the engine's buffer
};

// Subscribers per (row, kind). Rows are instrument slots for instrument data, bar keys for bars
// and feature ids for features. Delivery follows subscription order, which is part of the
// replayed state, so it is identical on replay. Storage is sized at construction; subscribing
// never allocates.
class SubscriptionMatrix {
public:
  SubscriptionMatrix(std::uint32_t rows, std::uint32_t per_cell)
      : rows_{rows}, per_cell_{per_cell},
        cells_{static_cast<std::size_t>(rows) * kDataKindCount * per_cell},
        counts_{static_cast<std::size_t>(rows) * kDataKindCount} {
    for (std::size_t i = 0; i < cells_.capacity(); ++i) {
      static_cast<void>(cells_.push_back(Subscriber{}));
    }
    for (std::size_t i = 0; i < counts_.capacity(); ++i) {
      static_cast<void>(counts_.push_back(0));
    }
  }

  // Adds the subscription of `strategy` to (row, kind), or changes its cadence (the buffer
  // index, if any, is kept; the engine reassigns it).
  [[nodiscard]] core::Status subscribe(std::uint32_t row, DataKind kind, StrategyIndex strategy,
                                       Cadence cadence) noexcept {
    if (row >= rows_ || !cadence.valid()) {
      return core::Status::InvalidArgument;
    }
    Subscriber* first = cell(row, kind);
    std::uint32_t& count = counts_[index(row, kind)];
    for (std::uint32_t i = 0; i < count; ++i) {
      if (first[i].strategy ==
          strategy) {               // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        first[i].cadence = cadence; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        first[i].last_period =
            UINT64_MAX; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return core::Status::Ok;
      }
    }
    if (count == per_cell_) {
      return core::Status::CapacityExceeded;
    }
    first[count++] =
        Subscriber{strategy, cadence}; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return core::Status::Ok;
  }

  // Removes the subscription, keeping the order of the others. NotFound when absent.
  [[nodiscard]] core::Status unsubscribe(std::uint32_t row, DataKind kind,
                                         StrategyIndex strategy) noexcept {
    if (row >= rows_) {
      return core::Status::InvalidArgument;
    }
    const std::span<Subscriber> subs = subscribers(row, kind);
    std::uint32_t& count = counts_[index(row, kind)];
    for (std::uint32_t i = 0; i < count; ++i) {
      if (subs[i].strategy == strategy) {
        for (std::uint32_t j = i + 1; j < count; ++j) {
          subs[j - 1] = subs[j];
        }
        --count;
        return core::Status::Ok;
      }
    }
    return core::Status::NotFound;
  }

  [[nodiscard]] std::span<Subscriber> subscribers(std::uint32_t row, DataKind kind) noexcept {
    if (row >= rows_) {
      return {};
    }
    return std::span<Subscriber>{cell(row, kind), counts_[index(row, kind)]};
  }

  // The subscription of `strategy` to (row, kind), or nullptr.
  [[nodiscard]] Subscriber* find(std::uint32_t row, DataKind kind,
                                 StrategyIndex strategy) noexcept {
    for (Subscriber& s : subscribers(row, kind)) {
      if (s.strategy == strategy) {
        return &s;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::uint32_t rows() const noexcept { return rows_; }

private:
  [[nodiscard]] static std::size_t index(std::uint32_t row, DataKind kind) noexcept {
    return static_cast<std::size_t>(row) * kDataKindCount + static_cast<std::size_t>(kind);
  }
  [[nodiscard]] Subscriber* cell(std::uint32_t row, DataKind kind) noexcept {
    return &cells_[index(row, kind) * per_cell_];
  }

  std::uint32_t rows_;
  std::uint32_t per_cell_;
  core::FixedVector<Subscriber> cells_;
  core::FixedVector<std::uint32_t> counts_;
};

} // namespace jarvis::data
