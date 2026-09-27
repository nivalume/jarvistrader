#include "jarvis/adapter/binance/exchange_info.hpp"

#include <algorithm>
#include <optional>

#include <simdjson.h>

#include "jarvis/core/int_math.hpp"
#include "jarvis/model/money.hpp"

namespace jarvis::adapter::binance {

namespace {

namespace dom = simdjson::dom;
using core::Status;

// The fewest decimals that represent a 10^9-scaled raw value exactly.
std::uint8_t minimal_precision(std::uint64_t magnitude) {
  std::uint8_t p = model::kFixedPrecision;
  while (p > 0 && magnitude % core::kPow10[std::size_t{model::kFixedPrecision} - p + 1] == 0) {
    --p;
  }
  return p;
}

std::optional<std::string_view> string_of(const dom::element& e, std::string_view key) {
  std::string_view v;
  if (e[key].get_string().get(v) != simdjson::SUCCESS) {
    return std::nullopt;
  }
  return v;
}

Status currency_of(std::string_view code, model::Currency& out) {
  if (core::ok(model::Currency::builtin(code, out))) {
    return Status::Ok;
  }
  // A coin nautilus has no built-in for: crypto with eight decimals, like the nautilus Binance
  // adapter's non-strict Currency.from_str.
  return model::Currency::create(code, 8, 0, code, model::CurrencyType::Crypto, out);
}

// value = percent / 100, written with the fewest decimals ("5.0000" -> 0.05).
Status fraction_of_percent(std::string_view percent, model::Decimal& out) {
  model::Decimal p;
  if (const Status s = model::Decimal::parse(percent, p); !core::ok(s)) {
    return s;
  }
  if (p.raw() % 100 != 0) {
    return Status::PrecisionLoss;
  }
  const std::int64_t raw = p.raw() / 100;
  return model::Decimal::from_raw(raw, minimal_precision(core::magnitude(raw)), out);
}

struct Filters {
  std::optional<std::string_view> tick, min_price, max_price, step, min_qty, max_qty, notional;
};

Status filters_of(const dom::element& symbol, Filters& f) {
  dom::array filters;
  if (symbol["filters"].get_array().get(filters) != simdjson::SUCCESS) {
    return Status::ParseError;
  }
  for (const dom::element filter : filters) {
    const std::optional<std::string_view> type = string_of(filter, "filterType");
    if (type == "PRICE_FILTER") {
      f.tick = string_of(filter, "tickSize");
      f.min_price = string_of(filter, "minPrice");
      f.max_price = string_of(filter, "maxPrice");
    } else if (type == "LOT_SIZE") {
      f.step = string_of(filter, "stepSize");
      f.min_qty = string_of(filter, "minQty");
      f.max_qty = string_of(filter, "maxQty");
    } else if (type == "MIN_NOTIONAL") {
      f.notional = string_of(filter, "notional");
    }
  }
  return f.tick && f.step ? Status::Ok : Status::ParseError;
}

// A limit of zero means "none" on Binance (a minPrice of "0").
template <typename T>
Status optional_limit(std::optional<std::string_view> text, std::uint8_t precision,
                      std::optional<T>& out) {
  if (!text) {
    return Status::Ok;
  }
  T v;
  if (const Status s = T::parse(*text, precision, v); !core::ok(s)) {
    return s;
  }
  if (!v.is_zero()) {
    out = v;
  }
  return Status::Ok;
}

Status definition_of(const dom::element& s, std::string_view symbol, core::UnixNanos ts,
                     PerpetualDefinition& out, std::string& error) {
  const auto fail = [&](Status st, std::string_view what) {
    error = std::string{symbol} + ": " + std::string{what};
    return st;
  };
  if (string_of(s, "contractType") != "PERPETUAL") {
    return fail(Status::InvalidArgument, "not a perpetual contract");
  }
  Filters f;
  if (!core::ok(filters_of(s, f))) {
    return fail(Status::ParseError, "missing PRICE_FILTER or LOT_SIZE");
  }
  model::InstrumentCommon c;
  model::Price tick;
  model::Quantity step;
  // filters_of guarantees both; an empty text would fail to parse anyway.
  if (!core::ok(model::Price::parse(f.tick.value_or(""), tick)) || !tick.is_positive() ||
      !core::ok(model::Quantity::parse(f.step.value_or(""), step)) || step.is_zero()) {
    return fail(Status::ParseError, "bad tickSize or stepSize");
  }
  c.price_precision = minimal_precision(core::magnitude(tick.raw()));
  c.size_precision = minimal_precision(step.raw());
  if (!core::ok(model::Price::from_raw(tick.raw(), c.price_precision, c.price_increment)) ||
      !core::ok(model::Quantity::from_raw(step.raw(), c.size_precision, c.size_increment)) ||
      !core::ok(model::Quantity::from_raw(core::kPow10[model::kFixedPrecision], 0, c.multiplier))) {
    return fail(Status::ParseError, "bad increments");
  }
  if (!core::ok(optional_limit(f.min_price, c.price_precision, c.min_price)) ||
      !core::ok(optional_limit(f.max_price, c.price_precision, c.max_price)) ||
      !core::ok(optional_limit(f.min_qty, c.size_precision, c.min_quantity)) ||
      !core::ok(optional_limit(f.max_qty, c.size_precision, c.max_quantity))) {
    return fail(Status::ParseError, "bad price or quantity limits");
  }
  if (!core::ok(perpetual_id(symbol, c.id)) ||
      !core::ok(model::Symbol::from(symbol, c.raw_symbol))) {
    return fail(Status::InvalidArgument, "the symbol is not a valid identifier");
  }
  const std::string_view quote =
      string_of(s, "marginAsset").value_or(string_of(s, "quoteAsset").value_or("USDT"));
  model::Currency base;
  if (!core::ok(currency_of(quote, c.quote_currency)) ||
      !core::ok(currency_of(string_of(s, "baseAsset").value_or(""), base))) {
    return fail(Status::InvalidArgument, "bad base or margin asset");
  }
  c.base_currency = base;
  c.settlement_currency = c.quote_currency;
  if (f.notional) {
    model::Decimal amount;
    model::Money notional;
    if (!core::ok(model::Decimal::parse(*f.notional, amount)) ||
        !core::ok(model::Money::from_raw(amount.raw(), c.quote_currency, notional))) {
      return fail(Status::ParseError, "bad MIN_NOTIONAL");
    }
    c.min_notional = notional;
  }
  if (!core::ok(fraction_of_percent(string_of(s, "requiredMarginPercent").value_or("5"),
                                    c.margin_init)) ||
      !core::ok(fraction_of_percent(string_of(s, "maintMarginPercent").value_or("2.5"),
                                    c.margin_maint))) {
    return fail(Status::ParseError, "bad margin percentages");
  }
  c.ts_event = ts;
  c.ts_init = ts;
  if (const Status v = model::validate(c); !core::ok(v)) {
    return fail(v, "the instrument does not validate");
  }
  out.instrument = model::CryptoPerpetual{c};
  out.trading = string_of(s, "status") == "TRADING";
  return Status::Ok;
}

} // namespace

Status perpetual_id(std::string_view symbol, model::InstrumentId& out) {
  std::string text{symbol};
  text += "-PERP.BINANCE";
  return model::InstrumentId::parse(text, out);
}

Status parse_exchange_info(std::string_view json, std::span<const std::string> symbols,
                           core::UnixNanos ts, std::vector<PerpetualDefinition>& out,
                           std::string& error) {
  out.clear();
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "exchangeInfo is not JSON";
    return Status::ParseError;
  }
  dom::array list;
  if (doc["symbols"].get_array().get(list) != simdjson::SUCCESS) {
    error = "exchangeInfo has no symbols array";
    return Status::ParseError;
  }
  std::vector<bool> found(symbols.size(), false);
  for (const dom::element s : list) {
    const std::optional<std::string_view> name = string_of(s, "symbol");
    if (!name) {
      error = "a symbol entry without a name";
      return Status::ParseError;
    }
    const auto wanted = std::find(symbols.begin(), symbols.end(), *name);
    if (!symbols.empty() && wanted == symbols.end()) {
      continue;
    }
    if (symbols.empty() && string_of(s, "contractType") != "PERPETUAL") {
      continue; // all perpetuals: skip dated contracts quietly
    }
    PerpetualDefinition d;
    if (const Status st = definition_of(s, *name, ts, d, error); !core::ok(st)) {
      return st;
    }
    if (wanted != symbols.end()) {
      found[static_cast<std::size_t>(wanted - symbols.begin())] = true;
    }
    out.push_back(d);
  }
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    if (!found[i]) {
      error = symbols[i] + " is not in the exchangeInfo response";
      return Status::NotFound;
    }
  }
  return Status::Ok;
}

} // namespace jarvis::adapter::binance
