#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"

namespace jarvis::model {

namespace detail {

class TextBuilder {
public:
  constexpr void put(std::string_view text) noexcept {
    for (const char c : text) {
      if (size_ < buffer_.size()) {
        buffer_[size_++] = c;
      }
    }
  }
  constexpr void put_u64(std::uint64_t value) noexcept {
    char digits[20]{};
    std::size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10U);
      value /= 10U;
    } while (value != 0);
    while (n > 0) {
      put(std::string_view{&digits[--n], 1});
    }
  }
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return std::string_view{buffer_.data(), size_};
  }

private:
  std::array<char, 200> buffer_{};
  std::size_t size_ = 0;
};

[[nodiscard]] constexpr core::Status parse_step(std::string_view text,
                                                std::uint64_t& out) noexcept {
  if (text.empty()) {
    return core::Status::ParseError;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return core::Status::ParseError;
    }
    if (value > (UINT64_MAX - 9U) / 10U) {
      return core::Status::Overflow;
    }
    value = value * 10U + static_cast<std::uint64_t>(c - '0');
  }
  if (value == 0) {
    return core::Status::InvalidArgument;
  }
  out = value;
  return core::Status::Ok;
}

} // namespace detail

// "{step}-{AGGREGATION}-{PRICE_TYPE}", e.g. "1-MINUTE-LAST".
struct BarSpecification {
  std::uint64_t step = 1;
  BarAggregation aggregation = BarAggregation::Minute;
  PriceType price_type = PriceType::Last;

  [[nodiscard]] static constexpr core::Status parse(std::string_view text,
                                                    BarSpecification& out) noexcept {
    const std::size_t first = text.find('-');
    const std::size_t second = first == std::string_view::npos ? first : text.find('-', first + 1);
    if (second == std::string_view::npos || text.find('-', second + 1) != std::string_view::npos) {
      return core::Status::ParseError;
    }
    BarSpecification spec;
    core::Status s = detail::parse_step(text.substr(0, first), spec.step);
    if (!core::ok(s)) {
      return s;
    }
    s = model::parse(text.substr(first + 1, second - first - 1), spec.aggregation);
    if (!core::ok(s)) {
      return s;
    }
    s = model::parse(text.substr(second + 1), spec.price_type);
    if (!core::ok(s)) {
      return s;
    }
    out = spec;
    return core::Status::Ok;
  }

  constexpr void write(detail::TextBuilder& b) const noexcept {
    b.put_u64(step);
    b.put("-");
    b.put(to_string(aggregation));
    b.put("-");
    b.put(to_string(price_type));
  }

  friend constexpr bool operator==(const BarSpecification&,
                                   const BarSpecification&) noexcept = default;
};

// "{instrument_id}-{step}-{AGG}-{PRICE_TYPE}-{SOURCE}", optionally followed by
// "@{step}-{AGG}-{SOURCE}" for a composite bar type aggregated from another one.
struct BarType {
  using Text = core::FixedString<200>;

  InstrumentId instrument_id;
  BarSpecification spec;
  AggregationSource aggregation_source = AggregationSource::External;
  bool composite = false;
  std::uint64_t composite_step = 0;
  BarAggregation composite_aggregation = BarAggregation::Minute;
  AggregationSource composite_aggregation_source = AggregationSource::Internal;

  [[nodiscard]] static constexpr core::Status parse(std::string_view text, BarType& out) noexcept {
    BarType value;
    const std::size_t at = text.find('@');
    std::string_view standard = text.substr(0, at);
    // The last four hyphen-separated fields are step, aggregation, price type and source.
    std::size_t cut = standard.size();
    for (int field = 0; field < 4; ++field) {
      cut = cut == 0 ? std::string_view::npos : standard.rfind('-', cut - 1);
      if (cut == std::string_view::npos) {
        return core::Status::ParseError;
      }
    }
    core::Status s = InstrumentId::parse(standard.substr(0, cut), value.instrument_id);
    if (!core::ok(s)) {
      return s;
    }
    const std::string_view rest = standard.substr(cut + 1);
    const std::size_t source_dash = rest.rfind('-');
    s = BarSpecification::parse(rest.substr(0, source_dash), value.spec);
    if (!core::ok(s)) {
      return s;
    }
    s = model::parse(rest.substr(source_dash + 1), value.aggregation_source);
    if (!core::ok(s)) {
      return s;
    }
    if (at != std::string_view::npos) {
      const std::string_view comp = text.substr(at + 1);
      const std::size_t d1 = comp.find('-');
      const std::size_t d2 = d1 == std::string_view::npos ? d1 : comp.find('-', d1 + 1);
      if (d2 == std::string_view::npos || comp.find('-', d2 + 1) != std::string_view::npos) {
        return core::Status::ParseError;
      }
      value.composite = true;
      s = detail::parse_step(comp.substr(0, d1), value.composite_step);
      if (!core::ok(s)) {
        return s;
      }
      s = model::parse(comp.substr(d1 + 1, d2 - d1 - 1), value.composite_aggregation);
      if (!core::ok(s)) {
        return s;
      }
      s = model::parse(comp.substr(d2 + 1), value.composite_aggregation_source);
      if (!core::ok(s)) {
        return s;
      }
    }
    out = value;
    return core::Status::Ok;
  }

  [[nodiscard]] constexpr Text text() const noexcept {
    detail::TextBuilder b;
    b.put(instrument_id.text().view());
    b.put("-");
    spec.write(b);
    b.put("-");
    b.put(to_string(aggregation_source));
    if (composite) {
      b.put("@");
      b.put_u64(composite_step);
      b.put("-");
      b.put(to_string(composite_aggregation));
      b.put("-");
      b.put(to_string(composite_aggregation_source));
    }
    Text out;
    static_cast<void>(Text::from(b.view(), out));
    return out;
  }

  friend constexpr bool operator==(const BarType&, const BarType&) noexcept = default;
};

struct Bar {
  BarType bar_type;
  Price open;
  Price high;
  Price low;
  Price close;
  Quantity volume;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init; // in backtests, the bar close time

  // high is the maximum and low the minimum of the four prices (nautilus Bar::new_checked).
  [[nodiscard]] static constexpr core::Status create(const BarType& type, Price open, Price high,
                                                     Price low, Price close, Quantity volume,
                                                     core::UnixNanos ts_event,
                                                     core::UnixNanos ts_init, Bar& out) noexcept {
    if (high < open || high < low || high < close || low > open || low > close) {
      return core::Status::InvalidArgument;
    }
    out = Bar{type, open, high, low, close, volume, ts_event, ts_init};
    return core::Status::Ok;
  }
};

} // namespace jarvis::model
