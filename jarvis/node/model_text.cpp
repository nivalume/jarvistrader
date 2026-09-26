#include "jarvis/node/model_text.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/decimal.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::node {

namespace m = jarvis::model;
using core::Status;

namespace {

std::string error(Status s) { return "ERROR " + std::string{core::to_string(s)}; }

template <typename T> std::string fixed(std::string_view text) {
  T value;
  const Status s = T::parse(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  std::array<char, m::kMaxDecimalText> buffer{};
  std::size_t n = 0;
  static_cast<void>(value.format(buffer, n));
  return std::string{buffer.data(), n} + " raw=" + std::to_string(value.raw()) +
         " precision=" + std::to_string(value.precision());
}

std::string money(std::string_view text) {
  m::Money value;
  const Status s = m::Money::parse(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  std::array<char, m::kMaxMoneyText> buffer{};
  std::size_t n = 0;
  static_cast<void>(value.format(buffer, n));
  return std::string{buffer.data(), n} + " raw=" + std::to_string(value.raw());
}

std::string currency(std::string_view text) {
  m::Currency value;
  const Status s = m::Currency::builtin(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  return std::string{value.code()} + " precision=" + std::to_string(value.precision()) +
         " iso4217=" + std::to_string(value.iso4217()) + " name=\"" + std::string{value.name()} +
         "\" type=" + std::string{m::to_string(value.currency_type())};
}

template <typename Id> std::string identifier(std::string_view text, bool with_tag) {
  Id value;
  const Status s = Id::from(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  std::string out{value.view()};
  if (with_tag) {
    out += " tag=";
    out += m::tag_of(value);
  }
  return out;
}

std::string account_id(std::string_view text) {
  m::AccountId value;
  const Status s = m::AccountId::from(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  return std::string{value.view()} + " issuer=" + std::string{m::issuer_of(value)};
}

std::string instrument_id(std::string_view text) {
  m::InstrumentId value;
  const Status s = m::InstrumentId::parse(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  return std::string{value.text().view()} + " symbol=" + std::string{value.symbol.view()} +
         " venue=" + std::string{value.venue.view()};
}

std::string bar_spec(std::string_view text) {
  m::BarSpecification value;
  const Status s = m::BarSpecification::parse(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  m::detail::TextBuilder b;
  value.write(b);
  return std::string{b.view()};
}

std::string bar_type(std::string_view text) {
  m::BarType value;
  const Status s = m::BarType::parse(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  return std::string{value.text().view()} + (value.composite ? " composite=true" : "");
}

std::string rfc3339_text(core::UnixNanos t) {
  std::array<char, core::kRfc3339MaxLength> buffer{};
  std::size_t n = 0;
  const Status s = core::format_rfc3339(t, buffer, n);
  return core::ok(s) ? std::string{buffer.data(), n} : error(s);
}

std::string unix_nanos(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return error(Status::ParseError);
  }
  return rfc3339_text(core::UnixNanos{value});
}

std::string rfc3339(std::string_view text) {
  core::UnixNanos value;
  const Status s = core::parse_rfc3339(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  return rfc3339_text(value) + " nanos=" + std::to_string(value.value());
}

std::string uuid(std::string_view text) {
  m::Uuid4 value;
  const Status s = m::Uuid4::parse(text, value);
  if (!core::ok(s)) {
    return error(s);
  }
  const m::Uuid4::Text out = value.text();
  return std::string{out.data(), out.size()};
}

std::string enumeration(std::string_view name, std::string_view text) {
  m::EnumValueText value;
  const Status s = m::parse_enum_by_name(name, text, value);
  if (s == Status::NotFound) {
    return "ERROR UnknownEnum";
  }
  if (!core::ok(s)) {
    return error(s);
  }
  return std::string{value.string} + " value=" + std::to_string(value.value);
}

} // namespace

std::string roundtrip_text(std::string_view type, std::string_view text) {
  constexpr std::string_view kEnumPrefix = "enum:";
  if (type.starts_with(kEnumPrefix)) {
    return enumeration(type.substr(kEnumPrefix.size()), text);
  }
  if (type == "price") {
    return fixed<m::Price>(text);
  }
  if (type == "quantity") {
    return fixed<m::Quantity>(text);
  }
  if (type == "decimal") {
    return fixed<m::Decimal>(text);
  }
  if (type == "money") {
    return money(text);
  }
  if (type == "currency") {
    return currency(text);
  }
  if (type == "instrument_id") {
    return instrument_id(text);
  }
  if (type == "symbol") {
    return identifier<m::Symbol>(text, false);
  }
  if (type == "venue") {
    return identifier<m::Venue>(text, false);
  }
  if (type == "trader_id") {
    return identifier<m::TraderId>(text, true);
  }
  if (type == "strategy_id") {
    return identifier<m::StrategyId>(text, true);
  }
  if (type == "account_id") {
    return account_id(text);
  }
  if (type == "client_order_id") {
    return identifier<m::ClientOrderId>(text, false);
  }
  if (type == "venue_order_id") {
    return identifier<m::VenueOrderId>(text, false);
  }
  if (type == "trade_id") {
    return identifier<m::TradeId>(text, false);
  }
  if (type == "position_id") {
    return identifier<m::PositionId>(text, false);
  }
  if (type == "bar_spec") {
    return bar_spec(text);
  }
  if (type == "bar_type") {
    return bar_type(text);
  }
  if (type == "unix_nanos") {
    return unix_nanos(text);
  }
  if (type == "rfc3339") {
    return rfc3339(text);
  }
  if (type == "uuid") {
    return uuid(text);
  }
  return "ERROR UnknownType";
}

std::string roundtrip_lines(std::string_view input) {
  std::string out;
  while (!input.empty()) {
    const std::size_t eol = input.find('\n');
    std::string_view line = input.substr(0, eol);
    input = eol == std::string_view::npos ? std::string_view{} : input.substr(eol + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty() || line.front() == '#') {
      out += line;
      out += '\n';
      continue;
    }
    const std::size_t space = line.find(' ');
    const std::string_view type = line.substr(0, space);
    const std::string_view text =
        space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
    out += line;
    out += " => ";
    out += roundtrip_text(type, text);
    out += '\n';
  }
  return out;
}

} // namespace jarvis::node
