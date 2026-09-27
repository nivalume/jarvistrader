#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <doctest/doctest.h>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/cost/fees.hpp"
#include "jarvis/cost/latency.hpp"
#include "jarvis/cost/slippage.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace c = jarvis::cost;
namespace m = jarvis::model;
using jarvis::core::Status;

m::Price px(std::string_view text) {
  m::Price out;
  REQUIRE(m::Price::parse(text, out) == Status::Ok);
  return out;
}
m::Quantity qty(std::string_view text) {
  m::Quantity out;
  REQUIRE(m::Quantity::parse(text, out) == Status::Ok);
  return out;
}
m::Decimal dec(std::string_view text) {
  m::Decimal out;
  REQUIRE(m::Decimal::parse(text, out) == Status::Ok);
  return out;
}
m::Money money(std::string_view text) {
  m::Money out;
  REQUIRE(m::Money::parse(text, out) == Status::Ok);
  return out;
}

m::InstrumentCommon linear(std::uint8_t price_precision = 1) {
  m::InstrumentCommon ic;
  m::Currency btc;
  m::Currency usdt;
  REQUIRE(m::Currency::builtin("BTC", btc) == Status::Ok);
  REQUIRE(m::Currency::builtin("USDT", usdt) == Status::Ok);
  ic.base_currency = btc;
  ic.quote_currency = usdt;
  ic.settlement_currency = usdt;
  ic.price_precision = price_precision;
  ic.size_precision = 3;
  REQUIRE(m::Quantity::parse("1", ic.multiplier) == Status::Ok);
  return ic;
}

c::MakerTakerFees fees(std::string_view name) {
  c::MakerTakerFees f;
  REQUIRE(c::MakerTakerFees::schedule(name, f) == Status::Ok);
  return f;
}

struct Level {
  m::Price price;
  m::Quantity size;
};

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("VIP0 fees on a linear perpetual, with and without the BNB discount") {
    const m::InstrumentCommon perp = linear();
    m::Money fee;
    REQUIRE(fees("binance_usdm_vip0")
                .commission(perp, m::LiquiditySide::Taker, qty("0.010"), px("65000.0"), fee) ==
            Status::Ok);
    CHECK(fee == money("0.325 USDT"));
    REQUIRE(fees("binance_usdm_vip0")
                .commission(perp, m::LiquiditySide::Maker, qty("0.010"), px("65000.0"), fee) ==
            Status::Ok);
    CHECK(fee == money("0.13 USDT"));
    REQUIRE(fees("binance_usdm_vip0_bnb")
                .commission(perp, m::LiquiditySide::Taker, qty("0.010"), px("65000.0"), fee) ==
            Status::Ok);
    CHECK(fee == money("0.2925 USDT"));
    REQUIRE(fees("binance_usdm_vip0")
                .commission(perp, m::LiquiditySide::NoLiquiditySide, qty("0.010"), px("65000.0"),
                            fee) == Status::Ok);
    CHECK(fee == money("0.325 USDT")); // unknown liquidity pays the taker rate
  }

  TEST_CASE("fees round up onto the currency grid and rebates round down") {
    const m::InstrumentCommon perp = linear(5);
    m::Money fee;
    // notional 1.23456789 USDT; taker 0.000617283945 -> 0.00061729
    REQUIRE(fees("binance_usdm_vip0")
                .commission(perp, m::LiquiditySide::Taker, qty("0.001"), px("1234.56789"), fee) ==
            Status::Ok);
    CHECK(fee == money("0.00061729 USDT"));
    c::MakerTakerFees rebate;
    REQUIRE(c::MakerTakerFees::create(dec("-0.0001"), dec("0.0004"), dec("0.1"), rebate) ==
            Status::Ok);
    REQUIRE(rebate.commission(perp, m::LiquiditySide::Maker, qty("0.001"), px("1234.56789"), fee) ==
            Status::Ok);
    CHECK(fee == money("-0.00012345 USDT")); // -0.000123456789, no discount on a rebate
  }

  TEST_CASE("schedules and rates are validated") {
    c::MakerTakerFees f;
    CHECK(c::MakerTakerFees::schedule("vip9", f) == Status::NotFound);
    CHECK(c::MakerTakerFees::create(dec("0.0002"), dec("0.0005"), dec("1"), f) ==
          Status::InvalidArgument);
    CHECK(c::MakerTakerFees::create(dec("0.0002"), dec("-0.0005"), dec("0"), f) ==
          Status::InvalidArgument);
    const c::MakerTakerFees zero = fees("zero");
    m::Money fee;
    REQUIRE(zero.commission(linear(), m::LiquiditySide::Taker, qty("1.000"), px("100.0"), fee) ==
            Status::Ok);
    CHECK(fee.is_zero());
  }

  TEST_CASE("funding: longs pay a positive rate to shorts") {
    const m::InstrumentCommon perp = linear();
    m::Money out;
    REQUIRE(c::funding_payment(perp, m::PositionSide::Long, qty("0.010"), px("65000.0"),
                               dec("0.0001"), out) == Status::Ok);
    CHECK(out == money("-0.065 USDT"));
    REQUIRE(c::funding_payment(perp, m::PositionSide::Short, qty("0.010"), px("65000.0"),
                               dec("0.0001"), out) == Status::Ok);
    CHECK(out == money("0.065 USDT"));
    REQUIRE(c::funding_payment(perp, m::PositionSide::Long, qty("0.010"), px("65000.0"),
                               dec("-0.0001"), out) == Status::Ok);
    CHECK(out == money("0.065 USDT"));
    REQUIRE(c::funding_payment(perp, m::PositionSide::Flat, qty("0.000"), px("65000.0"),
                               dec("0.0001"), out) == Status::Ok);
    CHECK(out.is_zero());
    CHECK(out.currency().code() == "USDT");
  }

  TEST_CASE("slippage walks the book and rounds the average against the taker") {
    const std::array<Level, 2> asks = {{{px("100.0"), qty("1.000")}, {px("100.5"), qty("2.000")}}};
    c::SlippageEstimate e;
    REQUIRE(c::BookDepthSlippage::estimate(m::OrderSide::Buy, std::span<const Level>{asks},
                                           qty("2.000"), e) == Status::Ok);
    CHECK(e.complete);
    CHECK(e.best == px("100.0"));
    CHECK(e.average == px("100.3")); // 100.25 rounded up for a buyer
    CHECK(e.filled == qty("2.000"));
    REQUIRE(c::BookDepthSlippage::estimate(m::OrderSide::Buy, std::span<const Level>{asks},
                                           qty("5.000"), e) == Status::Ok);
    CHECK_FALSE(e.complete);
    CHECK(e.filled == qty("3.000"));
    CHECK(e.average == px("100.4")); // 100.333...

    const std::array<Level, 2> bids = {{{px("99.5"), qty("1.000")}, {px("99.0"), qty("1.000")}}};
    REQUIRE(c::BookDepthSlippage::estimate(m::OrderSide::Sell, std::span<const Level>{bids},
                                           qty("2.000"), e) == Status::Ok);
    CHECK(e.average == px("99.2"));   // 99.25 rounded down for a seller
    CHECK(e.cost_raw == 600'000'000); // (99.5 - 99.2) x 2
    CHECK(c::BookDepthSlippage::estimate(m::OrderSide::Sell, std::span<const Level>{}, qty("1.000"),
                                         e) == Status::InvalidArgument);
  }

  TEST_CASE("latency draws are a function of seed, identity and hop") {
    const c::JitteredLatency a{7, 1'000, 2'000, 3'000, 500};
    const c::JitteredLatency b{7, 1'000, 2'000, 3'000, 500};
    const c::JitteredLatency other{8, 1'000, 2'000, 3'000, 500};
    bool differs = false;
    for (std::uint64_t id = 0; id < 200; ++id) {
      const auto d = a.delay(id, c::LatencyHop::Outbound);
      CHECK(d.value() >= 2'000);
      CHECK(d.value() <= 2'500);
      CHECK(d.value() == b.delay(id, c::LatencyHop::Outbound).value());
      differs = differs || d.value() != other.delay(id, c::LatencyHop::Outbound).value();
    }
    CHECK(differs);
    const c::JitteredLatency fixed{7, 1'000, 2'000, 3'000, 0};
    CHECK(fixed.delay(42, c::LatencyHop::Inbound).value() == 3'000);
    CHECK(fixed.delay(42, c::LatencyHop::Feed).value() == 1'000);
  }
}

TEST_SUITE("property") {
  TEST_CASE("a fee is the exact fee rounded up by less than one currency unit") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      const m::InstrumentCommon perp = linear(2);
      const c::MakerTakerFees f = fees("binance_usdm_vip0_bnb");
      m::Quantity q;
      m::Price p;
      REQUIRE(m::Quantity::from_raw(gen.range_u(1, 5'000'000) * 1'000'000, 3, q) == Status::Ok);
      REQUIRE(m::Price::from_raw(static_cast<std::int64_t>(gen.range_u(1, 10'000'000)) * 10'000'000,
                                 2, p) == Status::Ok);
      m::Money notional;
      REQUIRE(m::notional_value(perp, q, p, notional) == Status::Ok);
      m::Money fee;
      REQUIRE(f.commission(perp, m::LiquiditySide::Taker, q, p, fee) == Status::Ok);
      // exact = notional x 0.0005 x 0.9 at 10^27 scale; the fee must be >= and within 1e-8.
      const auto exact = static_cast<jarvis::core::u128>(notional.raw()) * 500'000U * 900'000'000U;
      const auto charged =
          static_cast<jarvis::core::u128>(fee.raw()) * 1'000'000'000'000'000'000ULL;
      CHECK(charged >= exact);
      CHECK(charged - exact < static_cast<jarvis::core::u128>(10) * 1'000'000'000'000'000'000ULL);
    });
  }
}
