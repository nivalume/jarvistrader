// Gating benchmarks for the model and the event log encoding (docs/architecture.md 17.3).
//
//   model/parse_decimal   Price::parse of venue price strings at the instrument precision
//   model/format_price    Price::format
//   model/notional_value  exact linear notional (128-bit multiply-divide)
//   log/append_record     encode a TradeTick record and append it to an in-memory segment
//                         buffer; the write(2) calls of the log writer are excluded
//   log/decode_record     CRC check plus decode of the same record

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "jarvis/core/event_key.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/wire.hpp"

namespace {

namespace m = jarvis::model;
namespace wire = jarvis::model::wire;
using jarvis::core::Status;

// Binance sends prices padded to eight decimals; the instrument precision is 1 for BTCUSDT.
constexpr std::array<std::string_view, 8> kPrices = {
    "65000.10000000", "64999.90000000",  "65012.30000000", "0.10000000",
    "65000.1",        "123456.70000000", "65001.00000000", "64000.50000000"};

void parse_decimal(benchmark::State& state) {
  std::size_t i = 0;
  for ([[maybe_unused]] auto _ : state) {
    m::Price p;
    Status s = m::Price::parse(kPrices[i++ & 7U], 1, p);
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(p);
  }
}

void format_price(benchmark::State& state) {
  m::Price p;
  static_cast<void>(m::Price::parse("65000.1", p));
  std::array<char, m::kMaxDecimalText> buffer{};
  for ([[maybe_unused]] auto _ : state) {
    std::size_t n = 0;
    benchmark::DoNotOptimize(p);
    Status s = p.format(buffer, n);
    benchmark::DoNotOptimize(s);
    benchmark::ClobberMemory();
  }
}

m::CryptoPerpetual btcusdt() {
  m::CryptoPerpetual perp;
  m::InstrumentCommon& c = perp.common;
  static_cast<void>(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", c.id));
  static_cast<void>(m::Currency::builtin("USDT", c.quote_currency));
  c.settlement_currency = c.quote_currency;
  static_cast<void>(m::Price::parse("0.1", c.price_increment));
  static_cast<void>(m::Quantity::parse("0.001", c.size_increment));
  static_cast<void>(m::Quantity::parse("1", c.multiplier));
  c.price_precision = 1;
  c.size_precision = 3;
  return perp;
}

void notional_value(benchmark::State& state) {
  const m::CryptoPerpetual perp = btcusdt();
  m::Quantity q;
  m::Price p;
  static_cast<void>(m::Quantity::parse("0.123", q));
  static_cast<void>(m::Price::parse("65000.1", p));
  for ([[maybe_unused]] auto _ : state) {
    m::Money out;
    benchmark::DoNotOptimize(q);
    Status s = m::notional_value(perp.common, q, p, out);
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(out);
  }
}

m::TradeTick sample_trade() {
  m::TradeTick t;
  static_cast<void>(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", t.instrument_id));
  static_cast<void>(m::Price::parse("65000.1", t.price));
  static_cast<void>(m::Quantity::parse("0.010", t.size));
  t.aggressor_side = m::AggressorSide::Buy;
  static_cast<void>(m::TradeId::from("4812765123", t.trade_id));
  t.ts_event = jarvis::core::UnixNanos{1'767'225'600'000'000'000ULL};
  t.ts_init = t.ts_event;
  return t;
}

void append_record(benchmark::State& state) {
  const m::Event event{sample_trade()};
  std::vector<std::byte> segment(1U << 20U);
  std::size_t used = 0;
  std::uint64_t seq = 0;
  for ([[maybe_unused]] auto _ : state) {
    if (segment.size() - used < 512) {
      used = 0; // a full segment buffer is handed to the writer thread; reuse it here
    }
    std::size_t written = 0;
    const jarvis::core::EventKey key{jarvis::core::UnixNanos{seq}, 1, seq};
    Status s = wire::encode_record(key, event, std::span{segment}.subspan(used), written);
    benchmark::DoNotOptimize(s);
    used += written;
    ++seq;
  }
  benchmark::ClobberMemory();
}

void decode_record(benchmark::State& state) {
  std::array<std::byte, 512> bytes{};
  std::size_t written = 0;
  static_cast<void>(
      wire::encode_record(jarvis::core::EventKey{}, m::Event{sample_trade()}, bytes, written));
  wire::DecodeScratch scratch{16};
  for ([[maybe_unused]] auto _ : state) {
    wire::RecordView view;
    m::Event event;
    benchmark::ClobberMemory();
    Status s = wire::decode_record(std::span<const std::byte>{bytes.data(), written}, view);
    if (jarvis::core::ok(s)) {
      s = wire::decode_event(view, scratch, event);
    }
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(event);
  }
}

} // namespace

BENCHMARK(parse_decimal)->Name("model/parse_decimal");
BENCHMARK(format_price)->Name("model/format_price");
BENCHMARK(notional_value)->Name("model/notional_value");
BENCHMARK(append_record)->Name("log/append_record");
BENCHMARK(decode_record)->Name("log/decode_record");
