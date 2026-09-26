#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <string_view>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/status.hpp"

namespace jarvis::model {

// Identifiers follow nautilus_trader's string rules (docs/architecture.md section 6.2).
// Capacities are jarvis choices sized for Binance and nautilus identifiers alike.

namespace detail {

[[nodiscard]] constexpr bool all_whitespace(std::string_view text) noexcept {
  return std::ranges::all_of(text, [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  });
}

[[nodiscard]] constexpr bool is_ascii(std::string_view text) noexcept {
  return std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) <= 0x7FU; });
}

// nautilus check_valid_string_utf8: not empty, not all whitespace.
[[nodiscard]] constexpr core::Status check_utf8(std::string_view text) noexcept {
  return text.empty() || all_whitespace(text) ? core::Status::InvalidArgument : core::Status::Ok;
}

// nautilus check_valid_string_ascii: additionally ASCII only.
[[nodiscard]] constexpr core::Status check_ascii(std::string_view text) noexcept {
  const core::Status s = check_utf8(text);
  if (!core::ok(s)) {
    return s;
  }
  return is_ascii(text) ? core::Status::Ok : core::Status::InvalidArgument;
}

// "{name}-{tag}" split at the last hyphen with both sides non-empty.
[[nodiscard]] constexpr core::Status check_name_tag(std::string_view text) noexcept {
  const core::Status s = check_ascii(text);
  if (!core::ok(s)) {
    return s;
  }
  const std::size_t hyphen = text.rfind('-');
  if (hyphen == std::string_view::npos || hyphen == 0 || hyphen + 1 == text.size()) {
    return core::Status::InvalidArgument;
  }
  return core::Status::Ok;
}

} // namespace detail

// A validated identifier string. `Rule` supplies the capacity and validation; distinct rules
// make distinct types, so a TraderId can never be passed where a StrategyId is expected.
template <typename Rule> class Identifier {
public:
  using Storage = core::FixedString<Rule::kCapacity>;

  constexpr Identifier() noexcept = default;

  [[nodiscard]] static constexpr core::Status from(std::string_view text,
                                                   Identifier& out) noexcept {
    const core::Status valid = Rule::validate(text);
    if (!core::ok(valid)) {
      return valid;
    }
    Identifier value;
    const core::Status stored = Storage::from(text, value.value_);
    if (!core::ok(stored)) {
      return stored;
    }
    out = value;
    return core::Status::Ok;
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept { return value_.view(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return value_.empty(); }

  friend constexpr bool operator==(const Identifier& a, const Identifier& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(const Identifier& a,
                                                    const Identifier& b) noexcept {
    return a.value_ <=> b.value_;
  }

private:
  Storage value_;
};

struct SymbolRule {
  static constexpr std::size_t kCapacity = 32;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    return detail::check_utf8(t);
  }
};
struct VenueRule {
  static constexpr std::size_t kCapacity = 16;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    return detail::check_ascii(t);
  }
};
struct TraderIdRule {
  static constexpr std::size_t kCapacity = 32;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    return detail::check_name_tag(t);
  }
};
struct StrategyIdRule {
  static constexpr std::size_t kCapacity = 32;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    return t == "EXTERNAL" ? core::Status::Ok : detail::check_name_tag(t);
  }
};
struct AccountIdRule {
  static constexpr std::size_t kCapacity = 32;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    const core::Status s = detail::check_ascii(t);
    if (!core::ok(s)) {
      return s;
    }
    const std::size_t hyphen = t.find('-');
    return hyphen == std::string_view::npos || hyphen == 0 || hyphen + 1 == t.size()
               ? core::Status::InvalidArgument
               : core::Status::Ok;
  }
};
template <std::size_t Capacity> struct AsciiRule {
  static constexpr std::size_t kCapacity = Capacity;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    return detail::check_ascii(t);
  }
};
struct ClientOrderIdRule : AsciiRule<36> {};
struct VenueOrderIdRule : AsciiRule<40> {};
struct ClientIdRule : AsciiRule<32> {};
struct ComponentIdRule : AsciiRule<32> {};
struct ExecAlgorithmIdRule : AsciiRule<32> {};
struct OrderListIdRule : AsciiRule<32> {};
struct TradeIdRule : AsciiRule<36> {};
struct PositionIdRule {
  static constexpr std::size_t kCapacity = 96;
  [[nodiscard]] static constexpr core::Status validate(std::string_view t) noexcept {
    return detail::check_utf8(t);
  }
};

using Symbol = Identifier<SymbolRule>;
using Venue = Identifier<VenueRule>;
using TraderId = Identifier<TraderIdRule>;
using StrategyId = Identifier<StrategyIdRule>;
using AccountId = Identifier<AccountIdRule>;
using ClientOrderId = Identifier<ClientOrderIdRule>;
using VenueOrderId = Identifier<VenueOrderIdRule>;
using ClientId = Identifier<ClientIdRule>;
using ComponentId = Identifier<ComponentIdRule>;
using ExecAlgorithmId = Identifier<ExecAlgorithmIdRule>;
using OrderListId = Identifier<OrderListIdRule>;
using TradeId = Identifier<TradeIdRule>;
using PositionId = Identifier<PositionIdRule>;

// The part after the last hyphen of a TraderId or StrategyId ("001" in "TESTER-001").
template <typename Id> [[nodiscard]] constexpr std::string_view tag_of(const Id& id) noexcept {
  const std::string_view text = id.view();
  const std::size_t hyphen = text.rfind('-');
  return hyphen == std::string_view::npos ? std::string_view{} : text.substr(hyphen + 1);
}

// The venue that issued an AccountId: the part before the first hyphen.
[[nodiscard]] constexpr std::string_view issuer_of(const AccountId& id) noexcept {
  const std::string_view text = id.view();
  return text.substr(0, text.find('-'));
}

// "{symbol}.{venue}", split at the last dot, so the symbol may contain dots.
struct InstrumentId {
  using Text = core::FixedString<SymbolRule::kCapacity + 1 + VenueRule::kCapacity>;

  Symbol symbol;
  Venue venue;

  [[nodiscard]] static constexpr core::Status create(const Symbol& symbol, const Venue& venue,
                                                     InstrumentId& out) noexcept {
    if (symbol.empty() || venue.empty()) {
      return core::Status::InvalidArgument;
    }
    out = InstrumentId{symbol, venue};
    return core::Status::Ok;
  }

  [[nodiscard]] static constexpr core::Status parse(std::string_view text,
                                                    InstrumentId& out) noexcept {
    const std::size_t dot = text.rfind('.');
    if (dot == std::string_view::npos) {
      return core::Status::ParseError;
    }
    InstrumentId value;
    core::Status s = Symbol::from(text.substr(0, dot), value.symbol);
    if (!core::ok(s)) {
      return s;
    }
    s = Venue::from(text.substr(dot + 1), value.venue);
    if (!core::ok(s)) {
      return s;
    }
    out = value;
    return core::Status::Ok;
  }

  [[nodiscard]] constexpr Text text() const noexcept {
    char buffer[Text::capacity()]{};
    std::size_t n = 0;
    for (const char c : symbol.view()) {
      buffer[n++] = c;
    }
    buffer[n++] = '.';
    for (const char c : venue.view()) {
      buffer[n++] = c;
    }
    Text out;
    static_cast<void>(Text::from(std::string_view{buffer, n}, out));
    return out;
  }

  friend constexpr bool operator==(const InstrumentId&, const InstrumentId&) noexcept = default;
  friend constexpr auto operator<=>(const InstrumentId&, const InstrumentId&) noexcept = default;
};

} // namespace jarvis::model
