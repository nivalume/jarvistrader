// The Binance USDⓈ-M adapter's codec and instrument loading (docs/architecture.md sections
// 13.3, 14.2 and 14.6). Fixtures under tests/data/binance/ were captured from the production
// market streams (fstream.binance.com) with jarvis-capture and from the testnet exchangeInfo.
// Set JARVIS_UPDATE_GOLDEN=1 to rewrite usdm_streams.expected after an intended change.

#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/json_codec.hpp"
#include "jarvis/adapter/binance/streams.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/node/event_text.hpp"

namespace {

namespace adapter = jarvis::adapter;
namespace binance = jarvis::adapter::binance;
namespace model = jarvis::model;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

std::string data_path(std::string_view name) {
  return std::string{JARVIS_SOURCE_DIR} + "/tests/data/binance/" + std::string{name};
}

std::string read_file(const std::string& path) {
  std::ifstream in{path, std::ios::binary};
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

template <typename T> std::string text(const T& value) {
  std::array<char, 96> buf{};
  std::size_t n = 0;
  REQUIRE(value.format(buf, n) == Status::Ok);
  return {buf.data(), n};
}

std::span<const std::byte> bytes(std::string_view s) {
  return std::as_bytes(std::span<const char>{s.data(), s.size()});
}

adapter::SymbolTable table() {
  adapter::SymbolTable t;
  for (const auto& [symbol, pp, sp] : {std::tuple{"BTCUSDT", 1, 3}, std::tuple{"ETHUSDT", 2, 3}}) {
    model::InstrumentId id;
    REQUIRE(binance::perpetual_id(symbol, id) == Status::Ok);
    REQUIRE(t.add(symbol,
                  adapter::SymbolEntry{
                      id, static_cast<std::uint8_t>(pp), static_cast<std::uint8_t>(sp), {}}) ==
            Status::Ok);
  }
  return t;
}

struct Decoded {
  Status status = Status::Ok;
  adapter::CollectingEmitter out;
};

Decoded decode(binance::JsonCodec& codec, std::string_view json, std::uint64_t recv = 7) {
  Decoded d;
  adapter::ConnCtx conn{1, UnixNanos{recv}};
  d.status = codec.decode(bytes(json), conn, d.out);
  return d;
}

template <typename T> std::string text(const std::optional<T>& value) {
  return value ? text(*value) : std::string{"None"};
}

std::string instrument_line(const binance::PerpetualDefinition& d) {
  const model::InstrumentCommon& c = d.instrument.common;
  std::ostringstream s;
  s << c.raw_symbol.view() << " id=" << c.id.text().view()
    << " price_precision=" << int{c.price_precision} << " size_precision=" << int{c.size_precision}
    << " price_increment=" << text(c.price_increment)
    << " size_increment=" << text(c.size_increment) << " min_price=" << text(c.min_price)
    << " max_price=" << text(c.max_price) << " min_quantity=" << text(c.min_quantity)
    << " max_quantity=" << text(c.max_quantity) << " min_notional=" << text(c.min_notional)
    << " margin_init=" << text(c.margin_init) << " margin_maint=" << text(c.margin_maint)
    << " base_currency=" << (c.base_currency ? c.base_currency->code() : "None")
    << " quote_currency=" << c.quote_currency.code()
    << " settlement_currency=" << c.settlement_currency.code();
  return s.str();
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("exchangeInfo loads perpetuals exactly as the Python catalog mapping does") {
    const std::string json = read_file(data_path("exchange_info_testnet.json"));
    std::vector<binance::PerpetualDefinition> defs;
    std::string error;
    const std::vector<std::string> wanted{"BTCUSDT", "ETHUSDT", "EOSUSDT"};
    REQUIRE(binance::parse_exchange_info(json, wanted, UnixNanos{5}, defs, error) == Status::Ok);
    REQUIRE(defs.size() == 3);
    std::istringstream expected{read_file(data_path("exchange_info_expected.txt"))};
    std::string line;
    std::size_t i = 0;
    while (std::getline(expected, line)) {
      if (line.empty() || line.front() == '#') {
        continue;
      }
      REQUIRE(i < defs.size());
      CHECK(instrument_line(defs[i]) == line);
      ++i;
    }
    CHECK(i == 3);
    CHECK(defs[0].trading);
    CHECK_FALSE(defs[2].trading); // EOSUSDT is SETTLING
    CHECK(defs[0].instrument.common.ts_event == UnixNanos{5});

    std::vector<binance::PerpetualDefinition> all;
    REQUIRE(binance::parse_exchange_info(json, {}, UnixNanos{}, all, error) == Status::Ok);
    CHECK(all.size() == 3); // the dated contract is skipped
    const std::vector<std::string> dated{"BTCUSDT_250627"};
    CHECK(binance::parse_exchange_info(json, dated, UnixNanos{}, all, error) ==
          Status::InvalidArgument);
    CHECK(error.find("not a perpetual") != std::string::npos);
    const std::vector<std::string> missing{"NOPEUSDT"};
    CHECK(binance::parse_exchange_info(json, missing, UnixNanos{}, all, error) == Status::NotFound);
    CHECK(binance::parse_exchange_info("{\"symbols\": 3}", {}, UnixNanos{}, all, error) ==
          Status::ParseError);
  }

  TEST_CASE("streams are split between the /public and /market routes") {
    CHECK(binance::route_of("btcusdt@bookTicker") == binance::Route::Public);
    CHECK(binance::route_of("btcusdt@depth@100ms") == binance::Route::Public);
    CHECK(binance::route_of("btcusdt@depth20@100ms") == binance::Route::Public);
    CHECK(binance::route_of("btcusdt@aggTrade") == binance::Route::Market);
    CHECK(binance::route_of("btcusdt@markPrice@1s") == binance::Route::Market);
    CHECK(binance::route_of("btcusdt@kline_1m") == binance::Route::Market);
    CHECK(binance::route_of("!forceOrder@arr") == binance::Route::Market);
    const std::vector<std::string> streams{"btcusdt@aggTrade", "btcusdt@bookTicker",
                                           "btcusdt@depth@100ms", "btcusdt@kline_1m"};
    const auto conns = binance::stream_connections("wss://fstream.binance.com", streams);
    REQUIRE(conns.size() == 2);
    CHECK(conns[0].url ==
          "wss://fstream.binance.com/public/stream?streams=btcusdt@bookTicker/btcusdt@depth@100ms");
    CHECK(conns[1].url ==
          "wss://fstream.binance.com/market/stream?streams=btcusdt@aggTrade/btcusdt@kline_1m");
    std::vector<std::string> many;
    many.reserve(1500);
    for (int i = 0; i < 1500; ++i) {
      many.push_back("s" + std::to_string(i) + "@aggTrade");
    }
    const auto split = binance::stream_connections("wss://x", many);
    REQUIRE(split.size() == 2);
    CHECK(split[0].streams.size() == binance::kMaxStreamsPerConnection);
    CHECK(split[1].streams.size() == 1500 - binance::kMaxStreamsPerConnection);
  }

  TEST_CASE("aggTrade becomes a TradeTick; the maker flag gives the aggressor") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    const Decoded d = decode(
        codec,
        R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1790526539033,"a":3466782974,"s":"BTCUSDT","p":"84551.10","q":"0.030","nq":"0.030","f":8123344783,"l":8123344783,"T":1790526538883,"m":true,"st":1}})",
        99);
    REQUIRE(d.status == Status::Ok);
    REQUIRE(d.out.events.size() == 1);
    const auto& trade = std::get<model::TradeTick>(d.out.events[0]);
    CHECK(trade.instrument_id.text().view() == "BTCUSDT-PERP.BINANCE");
    CHECK(text(trade.price) == "84551.1");
    CHECK(text(trade.size) == "0.030");
    CHECK(trade.aggressor_side == model::AggressorSide::Sell); // the buyer was the maker
    CHECK(trade.trade_id.view() == "3466782974");
    CHECK(trade.ts_event == UnixNanos{1790526538883ULL * 1'000'000});
    CHECK(trade.ts_init == UnixNanos{99});

    // A bare event (no combined-stream envelope) decodes the same way; m=false means BUY.
    const Decoded bare = decode(
        codec,
        R"({"e":"aggTrade","E":1,"a":5,"s":"ETHUSDT","p":"3000.25","q":"1.500","f":1,"l":1,"T":2,"m":false})");
    REQUIRE(bare.status == Status::Ok);
    CHECK(std::get<model::TradeTick>(bare.out.events.at(0)).aggressor_side ==
          model::AggressorSide::Buy);
  }

  TEST_CASE("bookTicker becomes a QuoteTick; stale update ids are dropped") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    const std::string_view q1 =
        R"({"e":"bookTicker","u":100,"s":"BTCUSDT","ps":"BTCUSDT","b":"84551.00","B":"2.646","a":"84551.10","A":"5.302","T":1790526538939,"E":1790526538940,"st":1})";
    const Decoded d = decode(codec, q1);
    REQUIRE(d.status == Status::Ok);
    const auto& quote = std::get<model::QuoteTick>(d.out.events.at(0));
    CHECK(text(quote.bid_price) == "84551.0");
    CHECK(text(quote.ask_price) == "84551.1");
    CHECK(text(quote.bid_size) == "2.646");
    CHECK(text(quote.ask_size) == "5.302");
    CHECK(quote.ts_event == UnixNanos{1790526538939ULL * 1'000'000});
    CHECK(decode(codec, q1).out.events.empty()); // the same update id again
    CHECK(codec.stats().stale_quotes == 1);
  }

  TEST_CASE("markPriceUpdate gives mark, index and funding updates") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    const Decoded d = decode(
        codec,
        R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1790526539000,"s":"BTCUSDT","p":"84551.10762319","ap":"84551.10762319","P":"84559.09090930","i":"84590.87304348","r":"0.00000801","T":1790553600000,"st":1}})");
    REQUIRE(d.status == Status::Ok);
    REQUIRE(d.out.events.size() == 3);
    CHECK(text(std::get<model::MarkPriceUpdate>(d.out.events[0]).value) == "84551.10762319");
    CHECK(text(std::get<model::IndexPriceUpdate>(d.out.events[1]).value) == "84590.87304348");
    const auto& funding = std::get<model::FundingRateUpdate>(d.out.events[2]);
    CHECK(text(funding.rate) == "0.00000801");
    CHECK(funding.next_funding_ns == UnixNanos{1790553600000ULL * 1'000'000});
    CHECK(funding.ts_event == UnixNanos{1790526539000ULL * 1'000'000});
  }

  TEST_CASE("kline: open klines are skipped, a closed one is a bar stamped at the close") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    const std::string open =
        R"({"e":"kline","E":1,"s":"BTCUSDT","k":{"t":1790526480000, "T":1790526539999, "s":"BTCUSDT", "i":"1m", "o":"84550.20", "c":"84551.10", "h":"84558.90", "l":"84536.70", "v":"104.194", "n":1483, "x":false}})";
    CHECK(decode(codec, open).out.events.empty());
    CHECK(codec.stats().open_klines == 1);
    std::string closed = open;
    closed.replace(closed.find("false"), 5, "true");
    const Decoded d = decode(codec, closed);
    REQUIRE(d.status == Status::Ok);
    const auto& bar = std::get<model::Bar>(d.out.events.at(0));
    CHECK(bar.bar_type.text().view() == "BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL");
    CHECK(text(bar.open) == "84550.2");
    CHECK(text(bar.high) == "84558.9");
    CHECK(text(bar.low) == "84536.7");
    CHECK(text(bar.close) == "84551.1");
    CHECK(text(bar.volume) == "104.194");
    CHECK(bar.ts_event == UnixNanos{1790526540000ULL * 1'000'000}); // open + 1 minute
  }

  TEST_CASE("forceOrder becomes a LiquidationOrder; other symbols are skipped") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    const Decoded d = decode(
        codec,
        R"({"stream":"btcusdt@forceOrder","data":{"e":"forceOrder","E":2,"o":{"s":"BTCUSDT","S":"SELL","o":"LIMIT","f":"IOC","q":"0.014","p":"84210.37","ap":"84251.6523","X":"FILLED","l":"0.014","z":"0.014","T":1790526546763}}})");
    REQUIRE(d.status == Status::Ok);
    const auto& liq = std::get<model::LiquidationOrder>(d.out.events.at(0));
    CHECK(liq.side == model::OrderSide::Sell);
    CHECK(text(liq.price) == "84210.37"); // off the tick: kept as sent
    CHECK(text(liq.average_price) == "84251.6523");
    CHECK(text(liq.quantity) == "0.014");
    CHECK(text(liq.filled_quantity) == "0.014");
    const Decoded other = decode(
        codec,
        R"({"stream":"!forceOrder@arr","data":{"e":"forceOrder","E":1790526547773,"o":{"s":"牛来USDT","S":"SELL","o":"LIMIT","f":"IOC","q":"9243","p":"0.1005200","ap":"0.1057892","X":"FILLED","l":"55","z":"9243","T":1790526546763,"ps":"牛来USDT","st":1}}})");
    CHECK(other.status == Status::Ok);
    CHECK(other.out.events.empty());
    CHECK(codec.stats().unknown_symbol == 1);
  }

  TEST_CASE("depthUpdate becomes a DepthDiff with exact levels") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    const Decoded d = decode(
        codec,
        R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":3,"T":1790526538981,"s":"BTCUSDT","ps":"BTCUSDT","U":11671659683245,"u":11671659687966,"pu":11671659683147,"b":[["84551.00","0.294"],["82548.60","0.000"]],"a":[["84551.10","5.302"]]}})",
        42);
    REQUIRE(d.status == Status::Ok);
    CHECK(d.out.events.empty());
    REQUIRE(d.out.depths.size() == 1);
    const adapter::DepthDiff& diff = d.out.depths[0].diff;
    CHECK(diff.first_update_id == 11671659683245ULL);
    CHECK(diff.final_update_id == 11671659687966ULL);
    CHECK(diff.prev_final_update_id == 11671659683147ULL);
    CHECK(diff.ts_event == UnixNanos{1790526538981ULL * 1'000'000});
    CHECK(diff.ts_init == UnixNanos{42});
    REQUIRE(diff.bids.size() == 2);
    CHECK(text(diff.bids[0].price) == "84551.0");
    CHECK(diff.bids[1].size.is_zero()); // removes the level
    REQUIRE(diff.asks.size() == 1);
    CHECK(text(diff.asks[0].size) == "5.302");
  }

  TEST_CASE("malformed, off-grid and unknown messages are refused with a reason") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    CHECK(decode(codec, R"({"result":null,"id":1})").status == Status::Ok);
    CHECK(codec.stats().control == 1);
    CHECK(decode(codec, "not json").status == Status::ParseError);
    CHECK(decode(codec, R"({"e":"aggTrade","a":5,"s":"BTCUSDT","q":"1","T":2,"m":false})").status ==
          Status::ParseError);
    CHECK(codec.error().find('p') != std::string::npos);
    // 84551.15 is not on BTCUSDT's 0.1 tick.
    CHECK(
        decode(codec,
               R"({"e":"aggTrade","a":5,"s":"BTCUSDT","p":"84551.15","q":"1.000","T":2,"m":false})")
            .status == Status::PrecisionLoss);
    CHECK(codec.error() == "aggTrade: bad price in p");
    CHECK(decode(codec, R"({"e":"somethingNew","s":"BTCUSDT"})").status ==
          Status::UnsupportedMessage);
    CHECK(decode(codec, R"({"x":1})").status == Status::UnsupportedMessage);
    CHECK(codec.stats().errors == 3);
  }
}

TEST_SUITE("golden") {
  TEST_CASE("captured USDⓈ-M streams decode to the recorded events") {
    const adapter::SymbolTable t = table();
    binance::JsonCodec codec{t};
    std::istringstream frames{read_file(data_path("usdm_streams.txt"))};
    std::string decoded;
    std::string line;
    while (std::getline(frames, line)) {
      const std::size_t space = line.find(' ');
      REQUIRE(space != std::string::npos);
      const std::uint64_t recv = std::stoull(line.substr(0, space));
      adapter::CollectingEmitter out;
      adapter::ConnCtx conn{0, UnixNanos{recv}};
      const Status s = codec.decode(bytes(std::string_view{line}.substr(space + 1)), conn, out);
      REQUIRE_MESSAGE(s == Status::Ok, codec.error() << " in " << line.substr(0, 120));
      for (const model::Event& e : out.events) {
        jarvis::node::append_event_text(decoded, e);
        decoded += '\n';
      }
      for (const auto& depth : out.depths) {
        const adapter::DepthDiff& d = depth.diff;
        decoded +=
            "DepthDiff " + std::string{d.instrument_id.text().view()} +
            " U=" + std::to_string(d.first_update_id) + " u=" + std::to_string(d.final_update_id) +
            " pu=" + std::to_string(d.prev_final_update_id) +
            " bids=" + std::to_string(d.bids.size()) + " asks=" + std::to_string(d.asks.size());
        if (!d.bids.empty()) {
          decoded +=
              " best_bid_level=" + text(d.bids.back().price) + "@" + text(d.bids.back().size);
        }
        decoded += '\n';
      }
    }
    const binance::CodecStats& st = codec.stats();
    CHECK(st.errors == 0);
    CHECK(st.unknown_symbol == 21); // the all-symbol liquidation stream
    CHECK(st.depth_diffs == 4);
    const std::string expected_path = data_path("usdm_streams.expected");
    const char* update = std::getenv("JARVIS_UPDATE_GOLDEN"); // NOLINT(concurrency-mt-unsafe)
    if (update != nullptr && std::string_view{update} == "1") {
      std::ofstream{expected_path, std::ios::binary} << decoded;
    }
    CHECK(decoded == read_file(expected_path));
  }
}
