// Fuzzes the text parsers of the model. Byte 0 selects a precision; the rest is the text.
// Properties (any violation traps):
//   - a value that parses formats to text that parses back to the same raw value and precision;
//   - a fixed-precision parse lands on that precision's grid;
//   - the same holds for Money, RFC 3339 timestamps, instrument ids and bar types.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"

namespace {

namespace m = jarvis::model;
using jarvis::core::ok;

void require(bool condition) {
  if (!condition) {
    __builtin_trap();
  }
}

template <typename T> void round_trip(std::string_view text, std::uint8_t precision) {
  std::array<char, 64> buffer{};
  T value;
  if (ok(T::parse(text, value))) {
    std::size_t n = 0;
    require(ok(value.format(buffer, n)));
    T back;
    require(ok(T::parse(std::string_view{buffer.data(), n}, back)));
    require(back.raw() == value.raw() && back.precision() == value.precision());
  }
  T fixed;
  if (ok(T::parse(text, precision, fixed))) {
    require(fixed.precision() == precision);
    T regrid;
    require(ok(T::from_raw(fixed.raw(), precision, regrid)));
  }
}

void money(std::string_view text) {
  m::Money value;
  if (!ok(m::Money::parse(text, value))) {
    return;
  }
  std::array<char, m::kMaxMoneyText> buffer{};
  std::size_t n = 0;
  require(ok(value.format(buffer, n)));
  m::Money back;
  require(ok(m::Money::parse(std::string_view{buffer.data(), n}, back)));
  require(back == value);
}

void timestamp(std::string_view text) {
  jarvis::core::UnixNanos t;
  if (!ok(jarvis::core::parse_rfc3339(text, t))) {
    return;
  }
  std::array<char, jarvis::core::kRfc3339MaxLength> buffer{};
  std::size_t n = 0;
  require(ok(jarvis::core::format_rfc3339(t, buffer, n)));
  jarvis::core::UnixNanos back;
  require(ok(jarvis::core::parse_rfc3339(std::string_view{buffer.data(), n}, back)));
  require(back == t);
}

void identifiers(std::string_view text) {
  m::InstrumentId id;
  if (ok(m::InstrumentId::parse(text, id))) {
    m::InstrumentId back;
    require(ok(m::InstrumentId::parse(id.text().view(), back)) && back == id);
  }
  m::BarType bar;
  if (ok(m::BarType::parse(text, bar))) {
    m::BarType back;
    require(ok(m::BarType::parse(bar.text().view(), back)) && back == bar);
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const auto precision = static_cast<std::uint8_t>(data[0] % 10U);
  const std::string_view text{reinterpret_cast<const char*>(data) + 1, size - 1}; // NOLINT
  round_trip<m::Price>(text, precision);
  round_trip<m::Quantity>(text, precision);
  money(text);
  timestamp(text);
  identifiers(text);
  return 0;
}
