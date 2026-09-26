#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/decimal.hpp"
#include "jarvis/model/fixed_point.hpp"

namespace jarvis::model {

inline constexpr std::int64_t kMoneyRawMax = kPriceRawMax;
inline constexpr std::int64_t kMoneyRawMin = kPriceRawMin;
// Longest Money text: amount, space, currency code.
inline constexpr std::size_t kMaxMoneyText = kMaxDecimalText + 1 + 16;

namespace detail {

[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

} // namespace detail

// An amount of a currency (nautilus `Money`): raw int64 at 10^9 scale on the currency's
// precision grid. Text form is "{amount} {code}".
class Money {
public:
  constexpr Money() noexcept = default;

  [[nodiscard]] static constexpr core::Status from_raw(std::int64_t raw, const Currency& currency,
                                                       Money& out) noexcept {
    if (raw < kMoneyRawMin || raw > kMoneyRawMax) {
      return core::Status::OutOfRange;
    }
    if (!detail::on_grid(core::magnitude(raw), currency.precision())) {
      return core::Status::PrecisionLoss;
    }
    out = Money{raw, currency};
    return core::Status::Ok;
  }

  // Truncates toward zero onto the currency's precision grid; never credits a sub-unit.
  [[nodiscard]] static constexpr core::Status
  from_raw_truncated(std::int64_t raw, const Currency& currency, Money& out) noexcept {
    const auto unit =
        static_cast<std::int64_t>(core::kPow10[kFixedPrecision - currency.precision()]);
    return from_raw(raw / unit * unit, currency, out);
  }

  // "{amount} {code}" with a built-in currency code, split on whitespace like nautilus
  // Money::from_str. The amount is rounded half to even at the currency's precision.
  [[nodiscard]] static constexpr core::Status parse(std::string_view text, Money& out) noexcept {
    std::string_view parts[2];
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
      while (pos < text.size() && detail::is_space(text[pos])) {
        ++pos;
      }
      const std::size_t start = pos;
      while (pos < text.size() && !detail::is_space(text[pos])) {
        ++pos;
      }
      if (pos > start) {
        if (count == 2) {
          return core::Status::ParseError;
        }
        parts[count++] = text.substr(start, pos - start);
      }
    }
    if (count != 2) {
      return core::Status::ParseError;
    }
    Currency currency;
    core::Status s = Currency::builtin(parts[1], currency);
    if (!core::ok(s)) {
      return s;
    }
    return parse_amount(parts[0], currency, out);
  }

  [[nodiscard]] static constexpr core::Status
  parse_amount(std::string_view amount, const Currency& currency, Money& out) noexcept {
    Price value;
    const core::Status s = Price::parse(amount, currency.precision(), value);
    if (!core::ok(s)) {
      return s;
    }
    return from_raw(value.raw(), currency, out);
  }

  [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr const Currency& currency() const noexcept { return currency_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }

  [[nodiscard]] constexpr core::Status format(std::span<char> out,
                                              std::size_t& written) const noexcept {
    std::size_t amount = 0;
    const core::Status s = detail::format_raw9(raw_, currency_.precision(), out, amount);
    if (!core::ok(s)) {
      return s;
    }
    const std::string_view code = currency_.code();
    if (amount + 1 + code.size() > out.size()) {
      return core::Status::OutOfRange;
    }
    out[amount] = ' ';
    for (std::size_t i = 0; i < code.size(); ++i) {
      out[amount + 1 + i] = code[i];
    }
    written = amount + 1 + code.size();
    return core::Status::Ok;
  }

  [[nodiscard]] static constexpr core::Status add(const Money& a, const Money& b,
                                                  Money& out) noexcept {
    std::int64_t raw = 0;
    if (a.currency_ != b.currency_) {
      return core::Status::InvalidArgument;
    }
    if (!core::checked_add(a.raw_, b.raw_, raw) || raw < kMoneyRawMin || raw > kMoneyRawMax) {
      return core::Status::Overflow;
    }
    out = Money{raw, a.currency_};
    return core::Status::Ok;
  }
  [[nodiscard]] static constexpr core::Status sub(const Money& a, const Money& b,
                                                  Money& out) noexcept {
    std::int64_t raw = 0;
    if (a.currency_ != b.currency_) {
      return core::Status::InvalidArgument;
    }
    if (!core::checked_sub(a.raw_, b.raw_, raw) || raw < kMoneyRawMin || raw > kMoneyRawMax) {
      return core::Status::Overflow;
    }
    out = Money{raw, a.currency_};
    return core::Status::Ok;
  }

  friend constexpr bool operator==(const Money& a, const Money& b) noexcept {
    return a.raw_ == b.raw_ && a.currency_ == b.currency_;
  }

private:
  constexpr Money(std::int64_t raw, const Currency& currency) noexcept
      : raw_{raw}, currency_{currency} {}

  std::int64_t raw_ = 0;
  Currency currency_;
};

// Balance of one currency in an account (nautilus `AccountBalance`): total == locked + free.
struct AccountBalance {
  Money total;
  Money locked;
  Money free;

  [[nodiscard]] static constexpr core::Status
  create(const Money& total, const Money& locked, const Money& free, AccountBalance& out) noexcept {
    Money sum;
    const core::Status s = Money::add(locked, free, sum);
    if (!core::ok(s)) {
      return s;
    }
    if (!(sum == total)) {
      return core::Status::InvalidArgument;
    }
    out = AccountBalance{total, locked, free};
    return core::Status::Ok;
  }

  [[nodiscard]] constexpr const Currency& currency() const noexcept { return total.currency(); }
};

} // namespace jarvis::model
