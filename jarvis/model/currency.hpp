#pragma once

#include <compare>
#include <cstdint>
#include <string_view>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/decimal.hpp"
#include "jarvis/model/generated/currencies.hpp"
#include "jarvis/model/generated/enums.hpp"

namespace jarvis::model {

// A currency (nautilus `Currency`). Two currencies are equal when their codes are equal.
class Currency {
public:
  using Code = core::FixedString<16>;
  using Name = core::FixedString<48>;

  constexpr Currency() noexcept = default;

  [[nodiscard]] static constexpr core::Status create(std::string_view code, std::uint8_t precision,
                                                     std::uint16_t iso4217, std::string_view name,
                                                     CurrencyType type, Currency& out) noexcept {
    if (code.empty() || precision > kFixedPrecision) {
      return core::Status::InvalidArgument;
    }
    Currency value;
    if (!core::ok(Code::from(code, value.code_)) || !core::ok(Name::from(name, value.name_))) {
      return core::Status::OutOfRange;
    }
    value.precision_ = precision;
    value.iso4217_ = iso4217;
    value.type_ = type;
    out = value;
    return core::Status::Ok;
  }

  // One of the nautilus built-in currencies, looked up by code.
  [[nodiscard]] static constexpr core::Status builtin(std::string_view code,
                                                      Currency& out) noexcept {
    for (const BuiltinCurrency& c : kBuiltinCurrencies) {
      if (c.code == code) {
        return create(c.code, c.precision, c.iso4217, c.name, c.type, out);
      }
    }
    return core::Status::NotFound;
  }

  [[nodiscard]] constexpr std::string_view code() const noexcept { return code_.view(); }
  [[nodiscard]] constexpr std::uint8_t precision() const noexcept { return precision_; }
  [[nodiscard]] constexpr std::uint16_t iso4217() const noexcept { return iso4217_; }
  [[nodiscard]] constexpr std::string_view name() const noexcept { return name_.view(); }
  [[nodiscard]] constexpr CurrencyType currency_type() const noexcept { return type_; }

  friend constexpr bool operator==(const Currency& a, const Currency& b) noexcept {
    return a.code_ == b.code_;
  }
  friend constexpr std::strong_ordering operator<=>(const Currency& a, const Currency& b) noexcept {
    return a.code_ <=> b.code_;
  }

private:
  Code code_;
  std::uint8_t precision_ = 0;
  std::uint16_t iso4217_ = 0;
  Name name_;
  CurrencyType type_ = CurrencyType::Crypto;
};

} // namespace jarvis::model
