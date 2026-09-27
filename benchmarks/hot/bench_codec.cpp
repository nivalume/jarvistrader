// Gating benchmarks for the Binance USDⓈ-M JSON codec (docs/architecture.md sections 13.3 and
// 17.3), on messages captured from the production streams (tests/data/binance/usdm_streams.txt).
//
//   codec/json_aggTrade     one aggTrade message to a TradeTick
//   codec/json_bookTicker   one bookTicker message to a QuoteTick (update ids increase)
//   codec/json_depth        one 1169-byte depthUpdate to a DepthDiff with its levels
//
// Each iteration includes the copy into simdjson's padded buffer and the emitter call.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/codec.hpp"

namespace {

namespace adapter = jarvis::adapter;
namespace binance = jarvis::adapter::binance;
using jarvis::core::Status;

constexpr std::string_view kAggTrade =
    R"json({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1790526539033,"a":3466782974,"s":"BTCUSDT","p":"84551.10","q":"0.030","nq":"0.030","f":8123344783,"l":8123344783,"T":1790526538883,"m":false,"st":1}})json";
constexpr std::string_view kBookTicker =
    R"json({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":11671659685961,"s":"BTCUSDT","ps":"BTCUSDT","b":"84551.00","B":"2.646","a":"84551.10","A":"5.302","T":1790526538939,"E":1790526538940,"st":1}})json";
constexpr std::string_view kDepth =
    R"json({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1790526538985,"T":1790526538981,"s":"BTCUSDT","ps":"BTCUSDT","U":11671659683245,"u":11671659687966,"pu":11671659683147,"b":[["1000.00","139.491"],["34051.00","0.294"],["34151.00","1.026"],["34251.00","1.008"],["34351.00","0.072"],["76095.90","0.002"],["80323.50","0.010"],["80323.60","0.008"],["81168.90","0.110"],["82548.60","0.000"],["82557.00","0.051"],["83263.70","0.600"],["83494.10","4.204"],["83705.50","5.856"],["84231.30","0.653"],["84241.80","0.000"],["84332.50","0.002"],["84333.30","0.035"],["84334.00","9.580"],["84335.40","0.019"],["84428.40","1.890"],["84526.00","0.434"],["84526.80","0.710"],["84538.00","0.003"],["84538.50","0.010"],["84540.50","1.187"],["84543.00","0.356"],["84543.10","0.003"],["84544.00","0.019"],["84544.10","0.001"],["84544.80","0.218"],["84544.90","0.001"],["84551.00","2.027"]],"a":[["84551.10","5.302"],["84559.50","0.017"],["84559.80","0.002"],["84562.00","0.947"],["84562.10","0.053"],["84565.90","0.000"],["84627.00","1.003"],["84627.10","20.105"],["84629.60","0.000"],["84629.90","0.001"],["84969.60","0.204"],["84973.90","0.279"],["85396.60","7.063"]],"st":1}})json";

class CountingEmitter final : public adapter::EventEmitter {
public:
  Status event(const jarvis::model::Event& /*e*/) override {
    ++events;
    return Status::Ok;
  }
  Status depth(const adapter::DepthDiff& d) override {
    levels += d.bids.size() + d.asks.size();
    return Status::Ok;
  }
  std::uint64_t events = 0;
  std::uint64_t levels = 0;
};

adapter::SymbolTable table() {
  adapter::SymbolTable t;
  jarvis::model::InstrumentId id;
  static_cast<void>(binance::perpetual_id("BTCUSDT", id));
  static_cast<void>(t.add("BTCUSDT", adapter::SymbolEntry{id, 1, 3, {}}));
  return t;
}

std::span<const std::byte> bytes(std::string_view s) {
  return std::as_bytes(std::span<const char>{s.data(), s.size()});
}

void run(benchmark::State& state, std::string_view message) {
  const adapter::SymbolTable t = table();
  binance::JsonCodec codec{t};
  CountingEmitter out;
  adapter::ConnCtx conn{0, jarvis::core::UnixNanos{1}};
  for ([[maybe_unused]] auto _ : state) {
    Status s = codec.decode(bytes(message), conn, out);
    benchmark::DoNotOptimize(s);
  }
  if (codec.stats().errors != 0) {
    state.SkipWithError(codec.error());
  }
  benchmark::DoNotOptimize(out.events);
  benchmark::DoNotOptimize(out.levels);
}

void json_agg_trade(benchmark::State& state) { run(state, kAggTrade); }

// Rewrites the update id each iteration so no quote is dropped as stale.
void json_book_ticker(benchmark::State& state) {
  const adapter::SymbolTable t = table();
  binance::JsonCodec codec{t};
  CountingEmitter out;
  adapter::ConnCtx conn{0, jarvis::core::UnixNanos{1}};
  std::string message{kBookTicker};
  const std::size_t at = message.find("\"u\":") + 4;
  const std::size_t end = message.find(',', at);
  const std::size_t width = end - at;
  std::uint64_t u = std::stoull(message.substr(at, width)); // same width while it grows
  for ([[maybe_unused]] auto _ : state) {
    std::string digits = std::to_string(++u);
    message.replace(at, width, digits.substr(digits.size() - width));
    Status s = codec.decode(bytes(message), conn, out);
    benchmark::DoNotOptimize(s);
  }
  if (codec.stats().errors != 0 || out.events == 0) {
    state.SkipWithError("bookTicker decode failed");
  }
}

void json_depth(benchmark::State& state) { run(state, kDepth); }

} // namespace

BENCHMARK(json_agg_trade)->Name("codec/json_aggTrade");
BENCHMARK(json_book_ticker)->Name("codec/json_bookTicker");
BENCHMARK(json_depth)->Name("codec/json_depth");
