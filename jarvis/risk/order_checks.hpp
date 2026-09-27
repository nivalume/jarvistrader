#pragma once

#include <string_view>

#include "jarvis/core/time.hpp"
#include "jarvis/execution/order_intent.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/instruments.hpp"

// Structural checks every order intent passes before the rule catalog of docs/architecture.md
// section 10.1 sees it: the order is well formed for its instrument (supported type and time in
// force, precisions, a price exactly when the type needs one). A failed check denies the order
// with one of the reason codes below, which go into OrderDenied.reason (CATEGORY_CONDITION).

namespace jarvis::risk {

namespace reason {

inline constexpr std::string_view kInstrumentUnknown = "INSTRUMENT_UNKNOWN";
inline constexpr std::string_view kOrderTypeUnsupported = "ORDER_TYPE_UNSUPPORTED";
inline constexpr std::string_view kTimeInForceUnsupported = "TIME_IN_FORCE_UNSUPPORTED";
inline constexpr std::string_view kPostOnlyInvalid = "POST_ONLY_INVALID";
inline constexpr std::string_view kQuantityNotPositive = "QUANTITY_NOT_POSITIVE";
inline constexpr std::string_view kQuantityInvalidPrecision = "QUANTITY_INVALID_PRECISION";
inline constexpr std::string_view kPriceMissing = "PRICE_MISSING";
inline constexpr std::string_view kPriceUnexpected = "PRICE_UNEXPECTED";
inline constexpr std::string_view kPriceNotPositive = "PRICE_NOT_POSITIVE";
inline constexpr std::string_view kPriceInvalidPrecision = "PRICE_INVALID_PRECISION";
inline constexpr std::string_view kExpireTimeMissing = "GTD_EXPIRE_TIME_MISSING";
inline constexpr std::string_view kGtdAlreadyExpired = "GTD_ALREADY_EXPIRED";
inline constexpr std::string_view kOmsCapacityExceeded = "OMS_CAPACITY_EXCEEDED";

} // namespace reason

// v1.0 orders: MARKET and LIMIT with GTC, IOC, FOK or GTD (section 2.3).
[[nodiscard]] constexpr bool supported_time_in_force(model::TimeInForce tif) noexcept {
  return tif == model::TimeInForce::Gtc || tif == model::TimeInForce::Ioc ||
         tif == model::TimeInForce::Fok || tif == model::TimeInForce::Gtd;
}

[[nodiscard]] constexpr std::string_view check_price(const model::InstrumentCommon& c,
                                                     model::Price price) noexcept {
  if (!price.is_positive()) {
    return reason::kPriceNotPositive;
  }
  if (price.precision() != c.price_precision) {
    return reason::kPriceInvalidPrecision;
  }
  return {};
}

[[nodiscard]] constexpr std::string_view check_quantity(const model::InstrumentCommon& c,
                                                        model::Quantity quantity) noexcept {
  if (quantity.is_zero()) {
    return reason::kQuantityNotPositive;
  }
  if (quantity.precision() != c.size_precision) {
    return reason::kQuantityInvalidPrecision;
  }
  return {};
}

// Empty when the intent is well formed.
[[nodiscard]] constexpr std::string_view check_intent(const model::InstrumentCommon& c,
                                                      const execution::OrderIntent& intent,
                                                      core::UnixNanos now) noexcept {
  const bool limit = intent.type == model::OrderType::Limit;
  if (!limit && intent.type != model::OrderType::Market) {
    return reason::kOrderTypeUnsupported;
  }
  if (!supported_time_in_force(intent.time_in_force)) {
    return reason::kTimeInForceUnsupported;
  }
  // Post-only rests or is rejected: LIMIT only, and never IOC or FOK.
  if (intent.post_only && (!limit || intent.time_in_force == model::TimeInForce::Ioc ||
                           intent.time_in_force == model::TimeInForce::Fok)) {
    return reason::kPostOnlyInvalid;
  }
  if (const std::string_view r = check_quantity(c, intent.quantity); !r.empty()) {
    return r;
  }
  if (limit && !intent.price) {
    return reason::kPriceMissing;
  }
  if (!limit && intent.price) {
    return reason::kPriceUnexpected;
  }
  if (intent.price) {
    if (const std::string_view r = check_price(c, *intent.price); !r.empty()) {
      return r;
    }
  }
  if (intent.time_in_force == model::TimeInForce::Gtd) {
    if (!intent.expire_time) {
      return reason::kExpireTimeMissing;
    }
    if (*intent.expire_time <= now) {
      return reason::kGtdAlreadyExpired;
    }
  }
  return {};
}

} // namespace jarvis::risk
