#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"

namespace jarvis::model {

// std::visit is not noexcept (bad_variant_access), so the visiting accessors below are not
// either; the variants here hold trivially copyable types and can never be valueless.

// Fields shared by every instrument (nautilus instrument definitions; `tick_scheme` and `info`
// are not carried). docs/architecture.md section 6.5.
struct InstrumentCommon {
  InstrumentId id;
  Symbol raw_symbol;
  std::optional<Currency> base_currency;
  Currency quote_currency;
  Currency settlement_currency;
  bool is_inverse = false;
  std::uint8_t price_precision = 0;
  std::uint8_t size_precision = 0;
  Price price_increment;
  Quantity size_increment;
  Quantity multiplier;
  std::optional<Quantity> lot_size;
  Decimal margin_init;
  Decimal margin_maint;
  std::optional<Quantity> max_quantity;
  std::optional<Quantity> min_quantity;
  std::optional<Money> max_notional;
  std::optional<Money> min_notional;
  std::optional<Price> max_price;
  std::optional<Price> min_price;
  core::UnixNanos ts_event;
  core::UnixNanos ts_init;
};

// nautilus validate_instrument_common: positive increments, multiplier and margins; increment
// precisions equal the instrument precisions; positive limits; min_price <= max_price.
[[nodiscard]] constexpr core::Status validate(const InstrumentCommon& c) noexcept {
  if (c.size_increment.is_zero() || c.size_increment.precision() != c.size_precision ||
      c.multiplier.is_zero() || !c.margin_init.is_positive() || !c.margin_maint.is_positive() ||
      !c.price_increment.is_positive() || c.price_increment.precision() != c.price_precision) {
    return core::Status::InvalidArgument;
  }
  if ((c.lot_size && c.lot_size->is_zero()) || (c.max_quantity && c.max_quantity->is_zero()) ||
      (c.min_quantity && c.min_quantity->is_zero())) {
    return core::Status::InvalidArgument;
  }
  if ((c.max_notional && c.max_notional->raw() <= 0) ||
      (c.min_notional && c.min_notional->raw() <= 0)) {
    return core::Status::InvalidArgument;
  }
  if (c.max_price &&
      (!c.max_price->is_positive() || c.max_price->precision() != c.price_precision)) {
    return core::Status::InvalidArgument;
  }
  if (c.min_price &&
      (!c.min_price->is_positive() || c.min_price->precision() != c.price_precision)) {
    return core::Status::InvalidArgument;
  }
  if (c.min_price && c.max_price && *c.min_price > *c.max_price) {
    return core::Status::InvalidArgument;
  }
  return core::Status::Ok;
}

// Spot pair (nautilus CurrencyPair): base_currency is required, settlement is the quote.
struct CurrencyPair {
  InstrumentCommon common;
};

// Perpetual swap (nautilus CryptoPerpetual), e.g. BTCUSDT-PERP.BINANCE.
struct CryptoPerpetual {
  InstrumentCommon common;
};

// Dated future (nautilus CryptoFuture). `underlying` replaces the base currency.
struct CryptoFuture {
  InstrumentCommon common;
  Currency underlying;
  core::UnixNanos activation_ns;
  core::UnixNanos expiration_ns;
};

using Instrument = std::variant<CurrencyPair, CryptoPerpetual, CryptoFuture>;

[[nodiscard]] constexpr const InstrumentCommon& common(const Instrument& instrument) {
  return std::visit([](const auto& i) -> const InstrumentCommon& { return i.common; }, instrument);
}

[[nodiscard]] constexpr InstrumentClass instrument_class(const Instrument& instrument) noexcept {
  switch (instrument.index()) {
  case 0:
    return InstrumentClass::Spot;
  case 1:
    return InstrumentClass::Swap;
  default:
    return InstrumentClass::Future;
  }
}

[[nodiscard]] constexpr AssetClass asset_class(const Instrument& instrument) noexcept {
  if (const auto* pair = std::get_if<CurrencyPair>(&instrument)) {
    const bool crypto = pair->common.quote_currency.currency_type() == CurrencyType::Crypto ||
                        (pair->common.base_currency &&
                         pair->common.base_currency->currency_type() == CurrencyType::Crypto);
    return crypto ? AssetClass::Cryptocurrency : AssetClass::FX;
  }
  return AssetClass::Cryptocurrency;
}

[[nodiscard]] constexpr core::Status validate(const Instrument& instrument) {
  const InstrumentCommon& c = common(instrument);
  if (std::holds_alternative<CurrencyPair>(instrument) && !c.base_currency) {
    return core::Status::InvalidArgument;
  }
  if (const auto* future = std::get_if<CryptoFuture>(&instrument)) {
    if (future->expiration_ns <= future->activation_ns) {
      return core::Status::InvalidArgument;
    }
  }
  return validate(c);
}

// Notional value of `quantity` at `price` (nautilus Instrument::calculate_notional_value):
//   linear:  quantity * multiplier * price, in the quote currency;
//   inverse: quantity * multiplier / price, in the base currency.
// Computed exactly in integers and truncated toward zero onto the currency's precision grid.
[[nodiscard]] constexpr core::Status notional_value(const InstrumentCommon& c, Quantity quantity,
                                                    Price price, Money& out) noexcept {
  std::uint64_t magnitude = 0;
  const std::uint64_t price_magnitude = core::magnitude(price.raw());
  core::Status s = core::Status::Ok;
  if (c.is_inverse) {
    if (price.is_zero() || !c.base_currency) {
      return core::Status::InvalidArgument;
    }
    s = core::mul_div_u64(quantity.raw(), c.multiplier.raw(), 1, price_magnitude, magnitude);
  } else {
    s = core::mul_div_u64(quantity.raw(), c.multiplier.raw(), price_magnitude,
                          static_cast<std::uint64_t>(kFixedScalar) *
                              static_cast<std::uint64_t>(kFixedScalar),
                          magnitude);
  }
  if (!core::ok(s)) {
    return s;
  }
  if (magnitude > static_cast<std::uint64_t>(kMoneyRawMax)) {
    return core::Status::Overflow;
  }
  const auto signed_raw = static_cast<std::int64_t>(magnitude);
  const std::int64_t raw = price.is_negative() && !c.is_inverse ? -signed_raw : signed_raw;
  const Currency& currency = c.is_inverse ? *c.base_currency : c.quote_currency;
  return Money::from_raw_truncated(raw, currency, out);
}

} // namespace jarvis::model
