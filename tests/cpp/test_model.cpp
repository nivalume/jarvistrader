#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <doctest/doctest.h>

#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/position_events.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/testkit/alloc.hpp"
#include "jarvis/testkit/property.hpp"

namespace m = jarvis::model;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::testkit::AllocationScope;
using jarvis::testkit::Gen;

namespace {

template <typename T> std::string text_of(const T& value) {
  std::array<char, 64> buffer{};
  std::size_t written = 0;
  REQUIRE(value.format(buffer, written) == Status::Ok);
  return std::string{buffer.data(), written};
}

m::Price price(std::string_view text) {
  m::Price p;
  REQUIRE(m::Price::parse(text, p) == Status::Ok);
  return p;
}

m::Quantity qty(std::string_view text) {
  m::Quantity q;
  REQUIRE(m::Quantity::parse(text, q) == Status::Ok);
  return q;
}

m::Currency currency(std::string_view code) {
  m::Currency c;
  REQUIRE(m::Currency::builtin(code, c) == Status::Ok);
  return c;
}

m::InstrumentId instrument_id(std::string_view text) {
  m::InstrumentId id;
  REQUIRE(m::InstrumentId::parse(text, id) == Status::Ok);
  return id;
}

m::CryptoPerpetual btcusdt_perp() {
  m::CryptoPerpetual perp;
  m::InstrumentCommon& c = perp.common;
  c.id = instrument_id("BTCUSDT-PERP.BINANCE");
  REQUIRE(m::Symbol::from("BTCUSDT", c.raw_symbol) == Status::Ok);
  c.base_currency = currency("BTC");
  c.quote_currency = currency("USDT");
  c.settlement_currency = currency("USDT");
  c.price_precision = 1;
  c.size_precision = 3;
  c.price_increment = price("0.1");
  c.size_increment = qty("0.001");
  c.multiplier = qty("1");
  REQUIRE(m::Decimal::parse("0.1", c.margin_init) == Status::Ok);
  REQUIRE(m::Decimal::parse("0.05", c.margin_maint) == Status::Ok);
  return perp;
}

} // namespace

// Event types must be trivially copyable so they can travel through SPSC rings by value.
static_assert(std::is_trivially_copyable_v<m::TradeTick>);
static_assert(std::is_trivially_copyable_v<m::QuoteTick>);
static_assert(std::is_trivially_copyable_v<m::OrderInitialized>);
static_assert(std::is_trivially_copyable_v<m::OrderFilled>);
static_assert(std::is_trivially_copyable_v<m::PositionClosed>);
static_assert(std::is_trivially_copyable_v<m::Event>);

TEST_SUITE("unit") {
  TEST_CASE("Price parses like nautilus and compares by raw value") {
    const m::Price p = price("1.23");
    CHECK(p.raw() == 1'230'000'000);
    CHECK(p.precision() == 2);
    CHECK(text_of(p) == "1.23");
    CHECK(price("1.230") == p);
    CHECK(price("1.230").precision() == 3);
    CHECK(price("-0.5").raw() == -500'000'000);
    CHECK(text_of(price("-0.5")) == "-0.5");
    CHECK(price("1_000.25").raw() == 1'000'250'000'000);
    CHECK(price("1.5e-3").precision() == 4);
    CHECK(price("1.5e-3").raw() == 1'500'000);
    CHECK(price("1e3").precision() == 0);
    CHECK(text_of(price("1e3")) == "1000");
    CHECK(text_of(price("0.000000001")) == "0.000000001");
    m::Price out;
    CHECK(m::Price::parse("1.0000000001", out) == Status::PrecisionLoss);
    CHECK(m::Price::parse("9223372037", out) == Status::OutOfRange);
    CHECK(m::Price::parse("", out) == Status::ParseError);
    CHECK(m::Price::parse("1.2.3", out) == Status::ParseError);
    CHECK(m::Price::parse(" 1", out) == Status::ParseError);
    CHECK(m::Price::parse("abc", out) == Status::ParseError);
  }

  TEST_CASE("Price at a fixed precision accepts extra zeros only") {
    m::Price p;
    CHECK(m::Price::parse("50000.10000000", 1, p) == Status::Ok);
    CHECK(p.raw() == 50'000'100'000'000);
    CHECK(text_of(p) == "50000.1");
    CHECK(m::Price::parse("50000.12", 1, p) == Status::PrecisionLoss);
    CHECK(m::Price::parse("7", 2, p) == Status::Ok);
    CHECK(text_of(p) == "7.00");
  }

  TEST_CASE("Price raw values must sit on the precision grid") {
    m::Price p;
    CHECK(m::Price::from_raw(1'230'000'000, 2, p) == Status::Ok);
    CHECK(m::Price::from_raw(1'234'000'000, 2, p) == Status::PrecisionLoss);
    CHECK(m::Price::from_raw(1, 10, p) == Status::InvalidArgument);
    CHECK(m::Price::from_raw(m::kPriceRawMax + 1, 9, p) == Status::OutOfRange);
    CHECK(m::Price::from_mantissa_exponent(123456, -2, 2, p) == Status::Ok);
    CHECK(text_of(p) == "1234.56");
    CHECK(m::Price::from_mantissa_exponent(123456, -3, 2, p) == Status::PrecisionLoss);
  }

  TEST_CASE("Price arithmetic keeps the larger precision and detects overflow") {
    m::Price sum;
    CHECK(m::Price::add(price("1.5"), price("0.25"), sum) == Status::Ok);
    CHECK(text_of(sum) == "1.75");
    CHECK(m::Price::sub(price("1.5"), price("2"), sum) == Status::Ok);
    CHECK(text_of(sum) == "-0.5");
    m::Price big;
    REQUIRE(m::Price::from_raw(m::kPriceRawMax, 0, big) == Status::Ok);
    CHECK(m::Price::add(big, price("1"), sum) == Status::Overflow);
  }

  TEST_CASE("Quantity is never negative") {
    m::Quantity q;
    CHECK(m::Quantity::parse("-1", q) == Status::OutOfRange);
    CHECK(m::Quantity::parse("18446744073", q) == Status::Ok);
    CHECK(m::Quantity::parse("18446744074", q) == Status::OutOfRange);
    CHECK(m::Quantity::sub(qty("1"), qty("2"), q) == Status::OutOfRange);
    CHECK(m::Quantity::add(qty("0.001"), qty("0.01"), q) == Status::Ok);
    CHECK(text_of(q) == "0.011");
  }

  TEST_CASE("Currency lookup uses the nautilus built-in table") {
    CHECK(currency("USDT").precision() == 8);
    CHECK(currency("USD").iso4217() == 840);
    CHECK(currency("USD").name() == "United States dollar");
    CHECK(currency("BTC").currency_type() == m::CurrencyType::Crypto);
    m::Currency c;
    CHECK(m::Currency::builtin("NOPE", c) == Status::NotFound);
    m::Currency custom;
    REQUIRE(m::Currency::create("USDT", 2, 0, "other", m::CurrencyType::Fiat, custom) == Status::Ok);
    CHECK(custom == currency("USDT")); // equality is by code
  }

  TEST_CASE("Money formats as amount and code") {
    m::Money money;
    CHECK(m::Money::parse("1000.00 USD", money) == Status::Ok);
    CHECK(text_of(money) == "1000.00 USD");
    CHECK(m::Money::parse("1000 USDT", money) == Status::Ok);
    CHECK(text_of(money) == "1000.00000000 USDT");
    CHECK(m::Money::parse("1.001 USD", money) == Status::PrecisionLoss);
    CHECK(m::Money::parse("1 XXX", money) == Status::NotFound);
    CHECK(m::Money::parse("1USD", money) == Status::ParseError);
    m::Money usd;
    m::Money usdt;
    REQUIRE(m::Money::parse("1.00 USD", usd) == Status::Ok);
    REQUIRE(m::Money::parse("1 USDT", usdt) == Status::Ok);
    m::Money sum;
    CHECK(m::Money::add(usd, usdt, sum) == Status::InvalidArgument);
    CHECK(m::Money::from_raw_truncated(-1'239'999'999, currency("USD"), sum) == Status::Ok);
    CHECK(text_of(sum) == "-1.23 USD"); // toward zero
  }

  TEST_CASE("AccountBalance requires total == locked + free") {
    m::Money total;
    m::Money locked;
    m::Money free;
    REQUIRE(m::Money::parse("10 USDT", total) == Status::Ok);
    REQUIRE(m::Money::parse("3 USDT", locked) == Status::Ok);
    REQUIRE(m::Money::parse("7 USDT", free) == Status::Ok);
    m::AccountBalance balance;
    CHECK(m::AccountBalance::create(total, locked, free, balance) == Status::Ok);
    CHECK(m::AccountBalance::create(total, locked, locked, balance) == Status::InvalidArgument);
  }

  TEST_CASE("Identifiers follow nautilus string rules") {
    const m::InstrumentId perp = instrument_id("BTCUSDT-PERP.BINANCE");
    CHECK(perp.symbol.view() == "BTCUSDT-PERP");
    CHECK(perp.venue.view() == "BINANCE");
    CHECK(perp.text().view() == "BTCUSDT-PERP.BINANCE");
    const m::InstrumentId dotted = instrument_id("ES.FUT.XCME");
    CHECK(dotted.symbol.view() == "ES.FUT");
    m::InstrumentId bad;
    CHECK(m::InstrumentId::parse("NODOT", bad) == Status::ParseError);
    CHECK(m::InstrumentId::parse("BTC.", bad) == Status::InvalidArgument);

    m::TraderId trader;
    CHECK(m::TraderId::from("TESTER-001", trader) == Status::Ok);
    CHECK(m::tag_of(trader) == "001");
    CHECK(m::TraderId::from("TESTER", trader) == Status::InvalidArgument);
    CHECK(m::TraderId::from("TESTER-", trader) == Status::InvalidArgument);
    m::StrategyId strategy;
    CHECK(m::StrategyId::from("EXTERNAL", strategy) == Status::Ok);
    CHECK(m::StrategyId::from("MyMM-001", strategy) == Status::Ok);
    m::AccountId account;
    CHECK(m::AccountId::from("BINANCE-001-USDM", account) == Status::Ok);
    CHECK(m::issuer_of(account) == "BINANCE");
    m::Venue venue;
    CHECK(m::Venue::from("   ", venue) == Status::InvalidArgument);
    CHECK(m::Venue::from("B\xC3\xA9", venue) == Status::InvalidArgument); // not ASCII
    m::Symbol symbol;
    CHECK(m::Symbol::from("B\xC3\xA9", symbol) == Status::Ok); // UTF-8 allowed
    m::TradeId trade;
    CHECK(m::TradeId::from(std::string(36, '1'), trade) == Status::Ok);
    CHECK(m::TradeId::from(std::string(37, '1'), trade) == Status::OutOfRange);
  }

  TEST_CASE("Enums keep nautilus values, strings and aliases") {
    CHECK(static_cast<int>(m::OrderStatus::Voided) == 15);
    CHECK(m::to_string(m::OrderStatus::PartiallyFilled) == "PARTIALLY_FILLED");
    for (const m::OrderStatus s : m::kOrderStatusValues) {
      m::OrderStatus parsed{};
      CHECK(m::parse(m::to_string(s), parsed) == Status::Ok);
      CHECK(parsed == s);
      m::OrderStatus by_value{};
      CHECK(m::from_value(static_cast<std::uint8_t>(s), by_value) == Status::Ok);
      CHECK(by_value == s);
    }
    m::OrderSide side{};
    CHECK(m::parse("buy", side) == Status::Ok); // ascii case-insensitive
    CHECK(side == m::OrderSide::Buy);
    m::AggressorSide aggressor{};
    CHECK(m::parse("BUYER", aggressor) == Status::Ok);
    CHECK(aggressor == m::AggressorSide::Buy);
    CHECK(m::to_string(aggressor) == "BUY");
    std::optional<m::OrderSide> none_side = m::OrderSide::Sell;
    CHECK(m::parse_optional("NO_ORDER_SIDE", none_side) == Status::Ok);
    CHECK_FALSE(none_side.has_value());
    CHECK(m::from_value(99, side) == Status::OutOfRange);
    CHECK(m::parse("HOLD", side) == Status::ParseError);
    CHECK(m::to_string(m::TrailingOffsetType::PriceUnits) == "PRICE");
    CHECK(m::to_string(m::BookType::L2_MBP) == "L2_MBP");
    CHECK(static_cast<int>(m::RecordFlag::F_LAST) == 128);
  }

  TEST_CASE("BarType strings round-trip, including composite bar types") {
    constexpr std::string_view kStandard = "BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL";
    m::BarType type;
    REQUIRE(m::BarType::parse(kStandard, type) == Status::Ok);
    CHECK(type.instrument_id == instrument_id("BTCUSDT-PERP.BINANCE"));
    CHECK(type.spec.step == 1);
    CHECK(type.spec.aggregation == m::BarAggregation::Minute);
    CHECK(type.spec.price_type == m::PriceType::Last);
    CHECK(type.aggregation_source == m::AggregationSource::External);
    CHECK(type.text().view() == kStandard);

    constexpr std::string_view kComposite =
        "BTCUSDT-PERP.BINANCE-5-MINUTE-LAST-INTERNAL@1-MINUTE-EXTERNAL";
    REQUIRE(m::BarType::parse(kComposite, type) == Status::Ok);
    CHECK(type.composite);
    CHECK(type.composite_step == 1);
    CHECK(type.text().view() == kComposite);

    CHECK(m::BarType::parse("BTCUSDT-PERP.BINANCE-0-MINUTE-LAST-EXTERNAL", type) ==
          Status::InvalidArgument);
    CHECK(m::BarType::parse("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST", type) != Status::Ok);
    m::Bar bar;
    CHECK(m::Bar::create(type, price("10"), price("9"), price("8"), price("9.5"), qty("1"),
                         UnixNanos{1}, UnixNanos{1}, bar) == Status::InvalidArgument);
    CHECK(m::Bar::create(type, price("10"), price("11"), price("8"), price("9.5"), qty("1"),
                         UnixNanos{1}, UnixNanos{1}, bar) == Status::Ok);
  }

  TEST_CASE("Market data constructors enforce nautilus invariants") {
    const m::InstrumentId id = instrument_id("BTCUSDT-PERP.BINANCE");
    m::QuoteTick quote;
    CHECK(m::QuoteTick::create(id, price("1.0"), price("1.01"), qty("1"), qty("1"), UnixNanos{},
                               UnixNanos{}, quote) == Status::InvalidArgument);
    CHECK(m::QuoteTick::create(id, price("1.00"), price("1.01"), qty("1"), qty("2"), UnixNanos{},
                               UnixNanos{}, quote) == Status::Ok);
    m::TradeId trade_id;
    REQUIRE(m::TradeId::from("123", trade_id) == Status::Ok);
    m::TradeTick trade;
    CHECK(m::TradeTick::create(id, price("1"), qty("0"), m::AggressorSide::Buy, trade_id,
                               UnixNanos{}, UnixNanos{}, trade) == Status::InvalidArgument);

    std::array<m::OrderBookDelta, 2> deltas{};
    deltas[0].instrument_id = id;
    deltas[0].sequence = 1;
    deltas[1].instrument_id = id;
    deltas[1].sequence = 2;
    deltas[1].flags = m::flag_bit(m::RecordFlag::F_LAST);
    m::OrderBookDeltas batch;
    REQUIRE(m::OrderBookDeltas::create(deltas, batch) == Status::Ok);
    CHECK(batch.sequence == 2);
    CHECK(m::has_flag(batch.flags, m::RecordFlag::F_LAST));
    deltas[1].instrument_id = instrument_id("ETHUSDT-PERP.BINANCE");
    CHECK(m::OrderBookDeltas::create(deltas, batch) == Status::InvalidArgument);
  }

  TEST_CASE("Instrument validation and classification") {
    m::Instrument perp = btcusdt_perp();
    CHECK(m::validate(perp) == Status::Ok);
    CHECK(m::instrument_class(perp) == m::InstrumentClass::Swap);
    CHECK(m::asset_class(perp) == m::AssetClass::Cryptocurrency);
    m::CryptoPerpetual broken = btcusdt_perp();
    broken.common.price_increment = price("0.10"); // precision 2 != price_precision 1
    CHECK(m::validate(m::Instrument{broken}) == Status::InvalidArgument);
    broken = btcusdt_perp();
    REQUIRE(m::Decimal::parse("0", broken.common.margin_init) == Status::Ok);
    CHECK(m::validate(m::Instrument{broken}) == Status::InvalidArgument);
  }

  TEST_CASE("Notional value for linear and inverse contracts") {
    const m::CryptoPerpetual perp = btcusdt_perp();
    m::Money notional;
    REQUIRE(m::notional_value(perp.common, qty("0.012"), price("50000.1"), notional) == Status::Ok);
    CHECK(text_of(notional) == "600.00120000 USDT");

    m::CryptoPerpetual inverse = btcusdt_perp();
    inverse.common.is_inverse = true;
    inverse.common.multiplier = qty("100");
    REQUIRE(m::notional_value(inverse.common, qty("10"), price("50000"), notional) == Status::Ok);
    CHECK(text_of(notional) == "0.02000000 BTC");
    REQUIRE(m::notional_value(inverse.common, qty("1"), price("3"), notional) == Status::Ok);
    CHECK(text_of(notional) == "33.33333333 BTC"); // truncated toward zero
  }

  TEST_CASE("Uuid4 is deterministic, versioned, and round-trips") {
    const jarvis::core::CounterRng rng{7};
    const m::Uuid4 a = m::Uuid4::derive(rng, 1, 0);
    CHECK(a == m::Uuid4::derive(rng, 1, 0));
    CHECK_FALSE(a == m::Uuid4::derive(rng, 2, 0));
    const m::Uuid4::Text text = a.text();
    const std::string_view view{text.data(), text.size()};
    CHECK(view[14] == '4');
    m::Uuid4 parsed;
    CHECK(m::Uuid4::parse(view, parsed) == Status::Ok);
    CHECK(parsed == a);
    CHECK(m::Uuid4::parse("2d89666b-1a1e-3a75-b193-4eb3b454c757", parsed) ==
          Status::InvalidArgument); // version 3
  }

  TEST_CASE("ClientOrderId generator is deterministic and decodable") {
    m::ClientOrderIdGenerator generator;
    REQUIRE(m::ClientOrderIdGenerator::create("mm01", 1, generator) == Status::Ok);
    m::ClientOrderId first;
    REQUIRE(generator.next(first) == Status::Ok);
    CHECK(first.view() == "mm01-000001-00000001");
    m::ClientOrderId second;
    REQUIRE(generator.next(second) == Status::Ok);
    CHECK(second.view() == "mm01-000001-00000002");
    m::DecodedClientOrderId decoded;
    REQUIRE(m::ClientOrderIdGenerator::decode(second, decoded) == Status::Ok);
    CHECK(decoded.node_tag.view() == "mm01");
    CHECK(decoded.epoch == 1);
    CHECK(decoded.seq == 2);
    m::ClientOrderId foreign;
    REQUIRE(m::ClientOrderId::from("O-20240101-000000-001-001-1", foreign) == Status::Ok);
    CHECK(m::ClientOrderIdGenerator::decode(foreign, decoded) == Status::NotFound);
    CHECK(m::ClientOrderIdGenerator::create("bad-tag", 1, generator) == Status::InvalidArgument);
    CHECK(m::ClientOrderIdGenerator::create("mm01", m::kMaxEpoch + 1, generator) ==
          Status::InvalidArgument);
  }
}

TEST_SUITE("property") {
  TEST_CASE("Price format then parse is the identity") {
    jarvis::testkit::for_all([](Gen& gen) {
      const auto precision = static_cast<std::uint8_t>(gen.range(0, 9));
      const auto unit = static_cast<std::int64_t>(jarvis::core::kPow10[9 - precision]);
      const std::int64_t raw = gen.range(m::kPriceRawMin / unit, m::kPriceRawMax / unit) * unit;
      m::Price p;
      REQUIRE(m::Price::from_raw(raw, precision, p) == Status::Ok);
      m::Price back;
      REQUIRE(m::Price::parse(text_of(p), back) == Status::Ok);
      CHECK(back.raw() == raw);
      CHECK(back.precision() == precision);
    });
  }

  TEST_CASE("Quantity format then parse at a lower precision drops only zeros") {
    jarvis::testkit::for_all([](Gen& gen) {
      const auto precision = static_cast<std::uint8_t>(gen.range(0, 8));
      const std::uint64_t unit = jarvis::core::kPow10[9 - precision];
      const std::uint64_t raw = gen.range_u(0, m::kQuantityRawMax / unit) * unit;
      m::Quantity wide;
      REQUIRE(m::Quantity::from_raw(raw, 9, wide) == Status::Ok);
      m::Quantity narrow;
      REQUIRE(m::Quantity::parse(text_of(wide), precision, narrow) == Status::Ok);
      CHECK(narrow.raw() == raw);
      CHECK(narrow.precision() == precision);
    });
  }

  TEST_CASE("Linear notional equals the exact product truncated to the currency grid") {
    jarvis::testkit::for_all([](Gen& gen) {
      const m::CryptoPerpetual perp = btcusdt_perp();
      const std::uint64_t q_units = gen.range_u(1, 1'000'000);          // thousandths of BTC
      const std::uint64_t p_units = gen.range_u(1, 1'000'000'000);      // tenths of USDT
      m::Quantity q;
      m::Price p;
      REQUIRE(m::Quantity::from_raw(q_units * 1'000'000ULL, 3, q) == Status::Ok);
      REQUIRE(m::Price::from_raw(static_cast<std::int64_t>(p_units * 100'000'000ULL), 1, p) ==
              Status::Ok);
      m::Money notional;
      const Status s = m::notional_value(perp.common, q, p, notional);
      // exact value in units of 10^-4 USDT: q_units * p_units
      const jarvis::core::u128 exact_raw = static_cast<jarvis::core::u128>(q_units) * p_units * 100'000U;
      if (exact_raw > static_cast<jarvis::core::u128>(m::kMoneyRawMax)) {
        CHECK(s == Status::Overflow);
      } else {
        REQUIRE(s == Status::Ok);
        CHECK(static_cast<jarvis::core::u128>(notional.raw()) == exact_raw);
      }
    });
  }

  TEST_CASE("ClientOrderId format and decode are inverses") {
    jarvis::testkit::for_all([](Gen& gen) {
      const std::uint64_t epoch = gen.range_u(0, m::kMaxEpoch);
      const std::uint64_t seq = gen.range_u(0, m::kMaxSeq);
      m::ClientOrderId id;
      REQUIRE(m::ClientOrderIdGenerator::format("n1", epoch, seq, id) == Status::Ok);
      m::DecodedClientOrderId decoded;
      REQUIRE(m::ClientOrderIdGenerator::decode(id, decoded) == Status::Ok);
      CHECK(decoded.epoch == epoch);
      CHECK(decoded.seq == seq);
    });
  }

  TEST_CASE("Uuid4 text round-trips") {
    jarvis::testkit::for_all([](Gen& gen) {
      const jarvis::core::CounterRng rng{gen.next()};
      const m::Uuid4 id = m::Uuid4::derive(rng, gen.next(), 3);
      const m::Uuid4::Text text = id.text();
      m::Uuid4 parsed;
      REQUIRE(m::Uuid4::parse(std::string_view{text.data(), text.size()}, parsed) == Status::Ok);
      CHECK(parsed == id);
    });
  }

  TEST_CASE("BarType text round-trips for every aggregation and price type") {
    jarvis::testkit::for_all([](Gen& gen) {
      m::BarType type;
      type.instrument_id = instrument_id("ETH-USD-SWAP.OKX");
      type.spec.step = gen.range_u(1, 1'000'000);
      type.spec.aggregation = gen.pick(std::span<const m::BarAggregation>{m::kBarAggregationValues});
      type.spec.price_type = gen.pick(std::span<const m::PriceType>{m::kPriceTypeValues});
      type.aggregation_source =
          gen.pick(std::span<const m::AggregationSource>{m::kAggregationSourceValues});
      m::BarType back;
      REQUIRE(m::BarType::parse(type.text().view(), back) == Status::Ok);
      CHECK(back == type);
    });
  }
}

TEST_SUITE("zero-alloc") {
  TEST_CASE("model parsing, formatting and notional math do not allocate") {
    const m::CryptoPerpetual perp = btcusdt_perp();
    m::ClientOrderIdGenerator generator;
    REQUIRE(m::ClientOrderIdGenerator::create("z0", 3, generator) == Status::Ok);
    std::array<char, 64> buffer{};
    std::uint64_t sink = 0;

    const AllocationScope scope;
    for (int i = 0; i < 100; ++i) {
      m::Price p;
      static_cast<void>(m::Price::parse("50123.4", 1, p));
      m::Quantity q;
      static_cast<void>(m::Quantity::parse("0.125", q));
      m::Money notional;
      static_cast<void>(m::notional_value(perp.common, q, p, notional));
      std::size_t written = 0;
      static_cast<void>(notional.format(buffer, written));
      m::InstrumentId id;
      static_cast<void>(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id));
      m::BarType bar;
      static_cast<void>(m::BarType::parse("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL", bar));
      m::ClientOrderId cid;
      static_cast<void>(generator.next(cid));
      sink += written + id.text().size() + bar.text().size() + cid.view().size();
    }
    const std::uint64_t counted = scope.allocations();
    CHECK(counted == 0U);
    CHECK(sink > 0U);
  }
}
