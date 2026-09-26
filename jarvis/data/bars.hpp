#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"

// Bar aggregation inside the kernel: INTERNAL bars built from trades (price type LAST) or quotes
// (BID, ASK, MID). Supported aggregations: TICK (every `step` updates), VOLUME (every `step`
// units; a trade that crosses the threshold is split across bars, as nautilus does) and time
// (MILLISECOND, SECOND, MINUTE, HOUR, DAY). Time bars cover [start, end) aligned to the Unix
// epoch and close at `end` with ts_event = ts_init = end; the engine arms a timer for
// next_close(), and an update at or after `end` also closes the bar first. Intervals without
// updates produce no bar.

namespace jarvis::data {

class BarAggregator {
public:
  BarAggregator() = default;

  [[nodiscard]] static core::Status create(const model::BarType& type, std::uint8_t price_precision,
                                           std::uint8_t size_precision,
                                           BarAggregator& out) noexcept {
    BarAggregator a;
    a.type_ = type;
    a.size_precision_ = size_precision;
    a.price_precision_ = type.spec.price_type == model::PriceType::Mid
                             ? static_cast<std::uint8_t>(price_precision + 1)
                             : price_precision;
    if (a.price_precision_ > model::kFixedPrecision ||
        type.aggregation_source != model::AggregationSource::Internal || type.spec.step == 0) {
      return core::Status::InvalidArgument;
    }
    switch (type.spec.aggregation) {
    case model::BarAggregation::Tick:
      a.mode_ = Mode::Tick;
      break;
    case model::BarAggregation::Volume:
      a.mode_ = Mode::Volume;
      if (type.spec.step > UINT64_MAX / static_cast<std::uint64_t>(model::kFixedScalar)) {
        return core::Status::OutOfRange;
      }
      a.volume_step_raw_ = type.spec.step * static_cast<std::uint64_t>(model::kFixedScalar);
      break;
    case model::BarAggregation::Millisecond:
    case model::BarAggregation::Second:
    case model::BarAggregation::Minute:
    case model::BarAggregation::Hour:
    case model::BarAggregation::Day: {
      a.mode_ = Mode::Time;
      const std::uint64_t unit = unit_ns(type.spec.aggregation);
      if (type.spec.step > UINT64_MAX / unit) {
        return core::Status::OutOfRange;
      }
      a.interval_ns_ = type.spec.step * unit;
      break;
    }
    case model::BarAggregation::TickImbalance:
    case model::BarAggregation::TickRuns:
    case model::BarAggregation::VolumeImbalance:
    case model::BarAggregation::VolumeRuns:
    case model::BarAggregation::Value:
    case model::BarAggregation::ValueImbalance:
    case model::BarAggregation::ValueRuns:
    case model::BarAggregation::Week:
    case model::BarAggregation::Month:
    case model::BarAggregation::Year:
    case model::BarAggregation::Renko:
      return core::Status::UnsupportedMessage;
    }
    out = a;
    return core::Status::Ok;
  }

  [[nodiscard]] const model::BarType& bar_type() const noexcept { return type_; }
  [[nodiscard]] bool uses_trades() const noexcept {
    return type_.spec.price_type == model::PriceType::Last;
  }
  [[nodiscard]] bool is_time_bar() const noexcept { return mode_ == Mode::Time; }

  // Close time of the open time bar, if any.
  [[nodiscard]] std::optional<core::UnixNanos> next_close() const noexcept {
    if (mode_ != Mode::Time || count_ == 0) {
      return std::nullopt;
    }
    return core::UnixNanos{window_end_};
  }

  // Feeds a trade (LAST bars). Completed bars are written to `out` (at most two: a time bar
  // closed by this update's timestamp, then tick or volume bars it completes); `completed` is
  // their number.
  template <std::size_t N>
  [[nodiscard]] core::Status on_trade(const model::TradeTick& t, std::array<model::Bar, N>& out,
                                      std::size_t& completed) noexcept {
    static_assert(N >= 2);
    completed = 0;
    if (!uses_trades()) {
      return core::Status::Ok;
    }
    return update(t.price, t.size.raw(), t.ts_event, t.ts_init, out, completed);
  }

  // Feeds a quote (BID, ASK or MID bars).
  template <std::size_t N>
  [[nodiscard]] core::Status on_quote(const model::QuoteTick& q, std::array<model::Bar, N>& out,
                                      std::size_t& completed) noexcept {
    static_assert(N >= 2);
    completed = 0;
    model::Price price;
    std::uint64_t size = 0;
    switch (type_.spec.price_type) {
    case model::PriceType::Bid:
      price = q.bid_price;
      size = q.bid_size.raw();
      break;
    case model::PriceType::Ask:
      price = q.ask_price;
      size = q.ask_size.raw();
      break;
    case model::PriceType::Mid: {
      const core::i128 mid = (static_cast<core::i128>(q.bid_price.raw()) + q.ask_price.raw()) / 2;
      const core::Status s =
          model::Price::from_raw(static_cast<std::int64_t>(mid), price_precision_, price);
      if (!core::ok(s)) {
        return s;
      }
      size = q.bid_size.raw() / 2 + q.ask_size.raw() / 2;
      break;
    }
    case model::PriceType::Last:
    case model::PriceType::Mark:
      return core::Status::Ok;
    }
    return update(price, size, q.ts_event, q.ts_init, out, completed);
  }

  // Closes the open time bar when `now` has reached its end (the engine's timer).
  [[nodiscard]] bool on_time(core::UnixNanos now, model::Bar& out) noexcept {
    if (mode_ != Mode::Time || count_ == 0 || now.value() < window_end_) {
      return false;
    }
    out = build(core::UnixNanos{window_end_});
    count_ = 0;
    return true;
  }

private:
  enum class Mode : std::uint8_t { Tick, Volume, Time };

  [[nodiscard]] static constexpr std::uint64_t unit_ns(model::BarAggregation a) noexcept {
    if (a == model::BarAggregation::Millisecond) {
      return 1'000'000ULL;
    }
    if (a == model::BarAggregation::Second) {
      return 1'000'000'000ULL;
    }
    if (a == model::BarAggregation::Minute) {
      return 60'000'000'000ULL;
    }
    if (a == model::BarAggregation::Hour) {
      return 3'600'000'000'000ULL;
    }
    return 86'400'000'000'000ULL; // Day
  }

  template <std::size_t N>
  [[nodiscard]] core::Status
  update(model::Price price, std::uint64_t size, core::UnixNanos ts_event, core::UnixNanos ts_init,
         std::array<model::Bar, N>& out, std::size_t& completed) noexcept {
    last_ts_ = ts_init;
    if (mode_ == Mode::Time) {
      if (count_ > 0 && ts_init.value() >= window_end_) {
        out[completed++] = build(core::UnixNanos{window_end_});
        count_ = 0;
      }
      if (count_ == 0) {
        const std::uint64_t start = ts_init.value() / interval_ns_ * interval_ns_;
        if (start > UINT64_MAX - interval_ns_) {
          return core::Status::OutOfRange;
        }
        window_end_ = start + interval_ns_;
      }
      static_cast<void>(ts_event);
      return add(price, size);
    }
    if (mode_ == Mode::Tick) {
      const core::Status s = add(price, size);
      if (core::ok(s) && count_ >= type_.spec.step) {
        out[completed++] = build(ts_init);
        count_ = 0;
      }
      return s;
    }
    // Volume: split the update across bars at the threshold.
    std::uint64_t remaining = size;
    do {
      const std::uint64_t room = volume_step_raw_ - volume_raw_;
      const std::uint64_t take = remaining < room ? remaining : room;
      const core::Status s = add(price, take);
      if (!core::ok(s)) {
        return s;
      }
      remaining -= take;
      if (volume_raw_ == volume_step_raw_) {
        if (completed == N) {
          return core::Status::CapacityExceeded;
        }
        out[completed++] = build(ts_init);
        count_ = 0;
      }
    } while (remaining > 0);
    return core::Status::Ok;
  }

  [[nodiscard]] core::Status add(model::Price price, std::uint64_t size) noexcept {
    if (count_ == 0) {
      open_ = high_ = low_ = close_ = price;
      volume_raw_ = 0;
    } else {
      high_ = price > high_ ? price : high_;
      low_ = price < low_ ? price : low_;
      close_ = price;
    }
    if (!core::checked_add(volume_raw_, size, volume_raw_)) {
      return core::Status::Overflow;
    }
    ++count_;
    return core::Status::Ok;
  }

  [[nodiscard]] model::Bar build(core::UnixNanos ts) const noexcept {
    model::Bar bar;
    bar.bar_type = type_;
    bar.open = at_precision(open_);
    bar.high = at_precision(high_);
    bar.low = at_precision(low_);
    bar.close = at_precision(close_);
    static_cast<void>(model::Quantity::from_raw(volume_raw_, size_precision_, bar.volume));
    bar.ts_event = ts;
    bar.ts_init = ts;
    return bar;
  }

  [[nodiscard]] model::Price at_precision(model::Price p) const noexcept {
    model::Price out;
    static_cast<void>(model::Price::from_raw(p.raw(), price_precision_, out));
    return out;
  }

  model::BarType type_;
  Mode mode_ = Mode::Tick;
  std::uint8_t price_precision_ = 0;
  std::uint8_t size_precision_ = 0;
  std::uint64_t volume_step_raw_ = 0;
  std::uint64_t interval_ns_ = 0;
  std::uint64_t window_end_ = 0;
  std::uint64_t count_ = 0;
  model::Price open_;
  model::Price high_;
  model::Price low_;
  model::Price close_;
  std::uint64_t volume_raw_ = 0;
  core::UnixNanos last_ts_;
};

} // namespace jarvis::data
