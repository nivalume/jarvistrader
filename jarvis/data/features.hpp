#pragma once

#include <cstddef>
#include <cstdint>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instrument_table.hpp"
#include "jarvis/model/outputs.hpp"

// Kernel features (docs/architecture.md section 7.5): indicators computed inside `step` in
// integer fixed point, so Python strategies can subscribe to a value instead of every tick.
// Every value is a model::Decimal at 10^9 scale (precision 9); divisions truncate toward zero.
//
//   Ema            exponential moving average of trade prices, alpha = 2 / (period + 1)
//   Vwap           volume-weighted average price of the last `window` trades
//   Imbalance      (bid_size - ask_size) / (bid_size + ask_size) of the latest quote, in [-1, 1]
//   Microprice     (bid * ask_size + ask * bid_size) / (bid_size + ask_size) of the latest quote
//   RealizedVol    sqrt(sum of squared simple returns) over the last `window` trade-to-trade
//                  returns; not annualized
//
// Declaring a feature sizes its window once; updates never allocate.

namespace jarvis::data {

enum class FeatureKind : std::uint8_t {
  Ema = 0,
  Vwap = 1,
  Imbalance = 2,
  Microprice = 3,
  RealizedVol = 4
};

struct FeatureSpec {
  FeatureKind kind = FeatureKind::Ema;
  model::InstrumentId instrument_id;
  std::uint32_t window = 0; // EMA period; VWAP and RealizedVol window in trades

  friend constexpr bool operator==(const FeatureSpec&, const FeatureSpec&) noexcept = default;
};

using model::FeatureId;
using model::FeatureUpdate;

[[nodiscard]] constexpr bool uses_trades(FeatureKind k) noexcept {
  return k == FeatureKind::Ema || k == FeatureKind::Vwap || k == FeatureKind::RealizedVol;
}

namespace detail {

[[nodiscard]] inline model::Decimal decimal9(core::i128 raw, core::Status& status) noexcept {
  model::Decimal out;
  if (raw > INT64_MAX || raw < INT64_MIN) {
    status = core::Status::Overflow;
    return out;
  }
  status = model::Decimal::from_raw(static_cast<std::int64_t>(raw), model::kFixedPrecision, out);
  return out;
}

} // namespace detail

// One declared feature and its state.
class Feature {
public:
  Feature() = default;

  [[nodiscard]] static core::Status create(const FeatureSpec& spec, model::InstrumentSlot slot,
                                           Feature& out) {
    const bool windowed = spec.kind == FeatureKind::Vwap || spec.kind == FeatureKind::RealizedVol;
    if ((spec.kind == FeatureKind::Ema || windowed) &&
        (spec.window == 0 || spec.window > (1U << 20U))) {
      return core::Status::InvalidArgument;
    }
    Feature f;
    f.spec_ = spec;
    f.slot_ = slot;
    if (windowed) {
      f.ring_ = core::FixedVector<Sample>{spec.window};
    }
    out = std::move(f);
    return core::Status::Ok;
  }

  [[nodiscard]] const FeatureSpec& spec() const noexcept { return spec_; }
  [[nodiscard]] model::InstrumentSlot slot() const noexcept { return slot_; }

  // Updates from a trade; `produced` tells whether a new value exists.
  [[nodiscard]] core::Status on_trade(const model::TradeTick& t, model::Decimal& value,
                                      bool& produced) noexcept {
    produced = false;
    core::Status s = core::Status::Ok;
    switch (spec_.kind) {
    case FeatureKind::Ema:
      if (!has_value_) {
        ema_raw_ = t.price.raw();
        has_value_ = true;
      } else {
        const core::i128 delta = static_cast<core::i128>(t.price.raw()) - ema_raw_;
        ema_raw_ +=
            static_cast<std::int64_t>(delta * 2 / (static_cast<core::i128>(spec_.window) + 1));
      }
      value = detail::decimal9(ema_raw_, s);
      break;
    case FeatureKind::Vwap:
      s = vwap(t, value);
      break;
    case FeatureKind::RealizedVol:
      s = realized_vol(t, value);
      if (s == core::Status::NotFound) {
        return core::Status::Ok; // first trade: no return yet
      }
      break;
    case FeatureKind::Imbalance:
    case FeatureKind::Microprice:
      return core::Status::Ok;
    }
    produced = core::ok(s);
    remember(produced, value);
    return s;
  }

  // Updates from a quote.
  [[nodiscard]] core::Status on_quote(const model::QuoteTick& q, model::Decimal& value,
                                      bool& produced) noexcept {
    produced = false;
    const core::u128 bs = q.bid_size.raw();
    const core::u128 as = q.ask_size.raw();
    if (bs + as == 0) {
      return core::Status::Ok;
    }
    core::Status s = core::Status::Ok;
    switch (spec_.kind) {
    case FeatureKind::Imbalance: {
      const core::i128 num = static_cast<core::i128>(bs) - static_cast<core::i128>(as);
      value = detail::decimal9(num * model::kFixedScalar / static_cast<core::i128>(bs + as), s);
      break;
    }
    case FeatureKind::Microprice: {
      // bid + spread * bid_size / (bid_size + ask_size), exact through 192 bits.
      const core::i128 spread = static_cast<core::i128>(q.ask_price.raw()) - q.bid_price.raw();
      std::uint64_t part = 0;
      s = core::mul_div_u64(core::magnitude(static_cast<std::int64_t>(spread)), q.bid_size.raw(), 1,
                            q.bid_size.raw() + q.ask_size.raw(), part);
      if (core::ok(s)) {
        const core::i128 signed_part = spread < 0 ? -static_cast<core::i128>(part) : part;
        value = detail::decimal9(q.bid_price.raw() + signed_part, s);
      }
      break;
    }
    case FeatureKind::Ema:
    case FeatureKind::Vwap:
    case FeatureKind::RealizedVol:
      return core::Status::Ok;
    }
    produced = core::ok(s);
    remember(produced, value);
    return s;
  }

  // The last value produced, if any (for state dumps; delivery does not read it).
  [[nodiscard]] bool last(model::Decimal& out) const noexcept {
    out = last_;
    return has_last_;
  }

private:
  void remember(bool produced, model::Decimal value) noexcept {
    if (produced) {
      last_ = value;
      has_last_ = true;
    }
  }

  struct Sample {
    std::int64_t price = 0;
    std::uint64_t size = 0;
    core::i128 contribution = 0; // price*size (VWAP) or squared return (RealizedVol)
  };

  // Adds `sample`, evicting the oldest when the window is full.
  void push(const Sample& sample) noexcept {
    if (ring_.size() < ring_.capacity()) {
      static_cast<void>(ring_.push_back(sample));
    } else {
      const Sample& old = ring_[head_];
      sum_contribution_ -= old.contribution;
      sum_size_ -= old.size;
      ring_[head_] = sample;
      head_ = (head_ + 1) % ring_.capacity();
    }
    sum_contribution_ += sample.contribution;
    sum_size_ += sample.size;
  }

  [[nodiscard]] core::Status vwap(const model::TradeTick& t, model::Decimal& value) noexcept {
    core::i128 pq = 0;
    if (__builtin_mul_overflow(static_cast<core::i128>(t.price.raw()),
                               static_cast<core::i128>(t.size.raw()), &pq)) {
      return core::Status::Overflow;
    }
    push(Sample{t.price.raw(), t.size.raw(), pq});
    if (sum_size_ == 0) {
      return core::Status::NotFound;
    }
    core::Status s = core::Status::Ok;
    value = detail::decimal9(sum_contribution_ / static_cast<core::i128>(sum_size_), s);
    return s;
  }

  [[nodiscard]] core::Status realized_vol(const model::TradeTick& t,
                                          model::Decimal& value) noexcept {
    const std::int64_t price = t.price.raw();
    if (!has_value_) {
      last_price_ = price;
      has_value_ = true;
      return core::Status::NotFound;
    }
    if (last_price_ == 0) {
      last_price_ = price;
      return core::Status::NotFound;
    }
    // Simple return at 10^9 scale, squared at 10^18 scale.
    const core::i128 r =
        (static_cast<core::i128>(price) - last_price_) * model::kFixedScalar / last_price_;
    last_price_ = price;
    core::i128 r2 = 0;
    if (__builtin_mul_overflow(r, r, &r2)) {
      return core::Status::Overflow;
    }
    push(Sample{price, 0, r2});
    core::Status s = core::Status::Ok;
    value = detail::decimal9(core::isqrt(static_cast<core::u128>(sum_contribution_)), s);
    return s;
  }

  FeatureSpec spec_;
  model::InstrumentSlot slot_;
  bool has_value_ = false;
  std::int64_t ema_raw_ = 0;
  std::int64_t last_price_ = 0;
  core::FixedVector<Sample> ring_{0};
  std::size_t head_ = 0;
  core::i128 sum_contribution_ = 0;
  core::u128 sum_size_ = 0;
  model::Decimal last_;
  bool has_last_ = false;
};

// The declared features. Identical specs share one feature (and one id).
class FeatureGraph {
public:
  explicit FeatureGraph(std::uint32_t capacity) : features_{capacity} {}

  // The id of the feature for `spec`, declaring it when new.
  [[nodiscard]] core::Status declare(const FeatureSpec& spec, model::InstrumentSlot slot,
                                     FeatureId& id) {
    for (std::size_t i = 0; i < features_.size(); ++i) {
      if (features_[i].spec() == spec) {
        id = static_cast<FeatureId>(i);
        return core::Status::Ok;
      }
    }
    if (features_.full()) {
      return core::Status::CapacityExceeded;
    }
    Feature f;
    const core::Status s = Feature::create(spec, slot, f);
    if (!core::ok(s)) {
      return s;
    }
    id = static_cast<FeatureId>(features_.size());
    return features_.push_back(std::move(f));
  }

  [[nodiscard]] std::size_t size() const noexcept { return features_.size(); }
  [[nodiscard]] Feature& at(FeatureId id) noexcept { return features_[id]; }
  [[nodiscard]] const Feature& at(FeatureId id) const noexcept { return features_[id]; }

private:
  core::FixedVector<Feature> features_;
};

} // namespace jarvis::data
