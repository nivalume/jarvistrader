#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/portfolio/margin.hpp"
#include "jarvis/portfolio/portfolio.hpp"
#include "jarvis/portfolio/position.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace m = jarvis::model;
namespace pf = jarvis::portfolio;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

constexpr std::int64_t kRaw = 1'000'000'000;

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
m::Currency ccy(std::string_view code) {
  m::Currency out;
  REQUIRE(m::Currency::builtin(code, out) == Status::Ok);
  return out;
}
m::ClientOrderId cid(std::string_view text) {
  m::ClientOrderId out;
  REQUIRE(m::ClientOrderId::from(text, out) == Status::Ok);
  return out;
}

m::CryptoPerpetual perpetual(bool inverse = false, std::string_view multiplier = "1") {
  m::CryptoPerpetual p;
  m::InstrumentCommon& c = p.common;
  REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", c.id) == Status::Ok);
  c.base_currency = ccy("BTC");
  c.quote_currency = ccy("USDT");
  c.settlement_currency = inverse ? ccy("BTC") : ccy("USDT");
  c.is_inverse = inverse;
  c.price_precision = 1;
  c.size_precision = 3;
  c.price_increment = px("0.1");
  c.size_increment = qty("0.001");
  c.multiplier = qty(multiplier);
  c.margin_init = dec("0.05");
  c.margin_maint = dec("0.025");
  return p;
}

// Applies a fill to a bare position and returns the realized PnL (raw).
std::int64_t fill(pf::NettingPosition& p, m::OrderSide side, std::string_view q,
                  std::string_view price, std::uint64_t multiplier = kRaw) {
  std::int64_t realized = 0;
  REQUIRE(p.apply(side, qty(q), px(price), multiplier, cid("C-1"), UnixNanos{1}, realized) ==
          Status::Ok);
  return realized;
}

struct Book {
  pf::Portfolio portfolio{pf::PortfolioConfig{4, 2, 8, pf::StandardMargin{}}};
  m::Instrument instrument{perpetual()};

  pf::FillOutcome fill(std::uint16_t strategy, m::OrderSide side, std::string_view q,
                       std::string_view price, std::optional<m::Money> commission = {}) {
    pf::FillOutcome out;
    REQUIRE(portfolio.on_fill(instrument, 0, strategy, side, qty(q), px(price), commission,
                              cid("C-1"), UnixNanos{10}, out) == Status::Ok);
    return out;
  }
  std::int64_t wallet(std::string_view code = "USDT") const {
    std::int64_t raw = 0;
    static_cast<void>(portfolio.wallet(ccy(code), raw));
    return raw;
  }
};

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("a long position averages its entries and realizes against the average") {
    pf::NettingPosition p;
    CHECK(fill(p, m::OrderSide::Buy, "1.000", "100.0") == 0);
    CHECK(fill(p, m::OrderSide::Buy, "1.000", "110.0") == 0);
    m::Price avg;
    REQUIRE(p.avg_px_open(avg));
    CHECK(avg == px("105.0"));
    CHECK(p.side() == m::PositionSide::Long);
    CHECK(fill(p, m::OrderSide::Sell, "1.000", "120.0") == 15 * kRaw);
    CHECK(fill(p, m::OrderSide::Sell, "1.000", "90.0") == -15 * kRaw);
    CHECK_FALSE(p.is_open());
    REQUIRE(p.avg_px_open(avg)); // kept after the close
    CHECK(avg == px("105.0"));
    REQUIRE(p.avg_px_close(avg));
    CHECK(avg == px("105.0"));
    CHECK(p.realized_raw() == 0);
    CHECK(p.peak_raw() == qty("2.000").raw());
  }

  TEST_CASE("shorts gain when the price falls; the multiplier scales PnL") {
    pf::NettingPosition p;
    fill(p, m::OrderSide::Sell, "2.000", "100.0");
    CHECK(fill(p, m::OrderSide::Buy, "1.000", "90.0") == 10 * kRaw);
    std::int64_t unrealized = 0;
    REQUIRE(p.unrealized(px("95.0"), kRaw, unrealized) == Status::Ok);
    CHECK(unrealized == 5 * kRaw);

    pf::NettingPosition big;
    fill(big, m::OrderSide::Buy, "1.000", "100.0", 10 * kRaw);
    CHECK(fill(big, m::OrderSide::Sell, "1.000", "101.0", 10 * kRaw) == 10 * kRaw);
  }

  TEST_CASE("a position refuses a reduction larger than itself; the portfolio splits flips") {
    pf::NettingPosition p;
    fill(p, m::OrderSide::Buy, "1.000", "100.0");
    std::int64_t realized = 0;
    CHECK(p.apply(m::OrderSide::Sell, qty("2.000"), px("100.0"), kRaw, cid("C-2"), UnixNanos{2},
                  realized) == Status::InvalidArgument);

    Book b;
    b.fill(0, m::OrderSide::Buy, "1.000", "100.0");
    const pf::FillOutcome out = b.fill(0, m::OrderSide::Sell, "3.000", "110.0");
    REQUIRE(out.count == 2);
    CHECK(out.parts[0].step == pf::PositionStep::Closed);
    CHECK(out.parts[0].quantity == qty("1.000"));
    CHECK(out.after[0].realized_raw() == 10 * kRaw);
    CHECK(out.parts[1].step == pf::PositionStep::Opened);
    CHECK(out.parts[1].quantity == qty("2.000"));
    CHECK(out.after[1].side() == m::PositionSide::Short);
    CHECK(b.portfolio.venue(0).signed_raw() == -qty("2.000").raw());
    CHECK(b.wallet() == 10 * kRaw);
  }

  TEST_CASE("balances follow account snapshots, realized PnL and commissions") {
    Book b;
    const std::array<m::AccountBalance, 1> balances = {
        m::AccountBalance{money("1000 USDT"), money("0 USDT"), money("1000 USDT")}};
    m::AccountState state;
    state.balances = balances;
    REQUIRE(b.portfolio.set_account(state) == Status::Ok);
    CHECK(b.wallet() == 1000 * kRaw);
    b.fill(0, m::OrderSide::Buy, "0.010", "65000.0", money("0.325 USDT"));
    CHECK(b.wallet() == 1000 * kRaw - 325'000'000);
    b.fill(0, m::OrderSide::Sell, "0.010", "65100.0", money("0.3255 USDT"));
    CHECK(b.wallet() == 1000 * kRaw - 325'000'000 + kRaw - 325'500'000);
    CHECK(b.portfolio.position(0, 0).total_commission_raw() == 650'500'000);
    b.fill(0, m::OrderSide::Buy, "0.001", "65000.0", money("0.001 BNB"));
    CHECK(b.wallet("BNB") == -1'000'000); // a commission in another currency moves that balance
    CHECK(b.portfolio.stats().fills == 3);
  }

  TEST_CASE("the ledger keeps each strategy's share of one venue position") {
    Book b;
    b.fill(0, m::OrderSide::Buy, "1.000", "100.0");
    b.fill(1, m::OrderSide::Sell, "1.000", "101.0");
    CHECK_FALSE(b.portfolio.venue(0).is_open()); // the account is flat
    CHECK(b.portfolio.position(0, 0).signed_raw() == qty("1.000").raw());
    CHECK(b.portfolio.position(1, 0).signed_raw() == -qty("1.000").raw());
    CHECK(b.wallet() == kRaw); // the venue realized 1 by selling above its entry
  }

  TEST_CASE("funding settles when the next funding time moves on, and from settled records") {
    Book b;
    b.fill(0, m::OrderSide::Buy, "0.010", "65000.0");
    b.fill(1, m::OrderSide::Sell, "0.004", "65000.0");
    b.portfolio.set_mark(0, px("65000.0"));
    const auto update = [](std::uint64_t ts, std::string_view rate,
                           std::optional<std::uint64_t> next) {
      m::FundingRateUpdate u;
      REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", u.instrument_id) == Status::Ok);
      u.rate = dec(rate);
      if (next) {
        u.next_funding_ns = UnixNanos{*next};
      }
      u.ts_event = UnixNanos{ts};
      u.ts_init = UnixNanos{ts};
      return u;
    };
    std::span<const pf::FundingShare> shares;
    REQUIRE(b.portfolio.on_funding(b.instrument, 0, update(10, "0.0001", 100), shares) ==
            Status::Ok);
    CHECK(shares.empty()); // the rate is predicted, nothing settles yet
    REQUIRE(b.portfolio.on_funding(b.instrument, 0, update(50, "0.0002", 100), shares) ==
            Status::Ok);
    CHECK(shares.empty());
    REQUIRE(b.portfolio.on_funding(b.instrument, 0, update(101, "0.0003", 200), shares) ==
            Status::Ok);
    REQUIRE(shares.size() == 2); // settles 0.0002, the last rate before the funding time
    CHECK(shares[0].strategy == 0);
    CHECK(shares[0].payment == money("-0.13 USDT"));
    CHECK(shares[1].payment == money("0.052 USDT"));
    CHECK(b.wallet() == -78'000'000); // the venue is long 0.006
    REQUIRE(b.portfolio.on_funding(b.instrument, 0, update(300, "-0.0001", std::nullopt), shares) ==
            Status::Ok);
    REQUIRE(shares.size() == 2);
    CHECK(shares[0].payment == money("0.065 USDT"));
    CHECK(b.portfolio.position(0, 0).funding_raw() == -65'000'000);
    CHECK(b.portfolio.stats().funding_settlements == 2);
  }

  TEST_CASE("inverse contracts are not booked yet") {
    Book b;
    b.instrument = m::Instrument{perpetual(true)};
    const pf::FillOutcome out = b.fill(0, m::OrderSide::Buy, "1.000", "100.0");
    CHECK_FALSE(out.booked);
    CHECK(b.portfolio.stats().unsupported_fills == 1);
    CHECK_FALSE(b.portfolio.venue(0).is_open());
  }

  TEST_CASE("margins: fixed fractions or leverage, rounded up") {
    const m::CryptoPerpetual p = perpetual();
    m::Money out;
    REQUIRE(pf::StandardMargin::initial(p.common, qty("0.010"), px("65000.0"), out) == Status::Ok);
    CHECK(out == money("32.5 USDT"));
    REQUIRE(pf::StandardMargin::maintenance(p.common, qty("0.010"), px("65000.0"), out) ==
            Status::Ok);
    CHECK(out == money("16.25 USDT"));
    const pf::LeveragedMargin x125{125};
    REQUIRE(x125.initial(p.common, qty("0.010"), px("65000.1"), out) == Status::Ok);
    CHECK(out == money("5.2000080 USDT")); // 650.001 / 125 = 5.200008
    const pf::LeveragedMargin x7{7};
    REQUIRE(x7.initial(p.common, qty("0.001"), px("100.0"), out) == Status::Ok);
    CHECK(out == money("0.01428572 USDT")); // 0.1 / 7 = 0.0142857142..., rounded up

    Book b;
    b.fill(0, m::OrderSide::Buy, "0.010", "65000.0");
    m::Money initial;
    m::Money maintenance;
    REQUIRE(b.portfolio.margins(p.common, 0, initial, maintenance) == Status::Ok);
    CHECK(initial == money("32.5 USDT")); // no mark yet: valued at the average open price
    b.portfolio.set_mark(0, px("66000.0"));
    REQUIRE(b.portfolio.margins(p.common, 0, initial, maintenance) == Status::Ok);
    CHECK(initial == money("33 USDT"));
  }
}

TEST_SUITE("property") {
  TEST_CASE("a round trip realizes the difference of the exit and entry notionals") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      pf::NettingPosition p;
      jarvis::core::i128 bought = 0; // sum of px.raw x qty.raw of buys
      jarvis::core::i128 sold = 0;
      std::int64_t realized = 0;
      std::int64_t closes = 0;
      for (int i = 0; i < 40; ++i) {
        const m::OrderSide side = gen.coin() ? m::OrderSide::Buy : m::OrderSide::Sell;
        std::uint64_t q = gen.range_u(1, 5000) * 1'000'000; // 0.001 .. 5.000
        const std::uint64_t closable = p.closable(side);
        if (closable != 0 && q > closable) {
          q = closable; // flips are split by the portfolio, not here
        }
        const auto price = static_cast<std::int64_t>(gen.range_u(90'000, 110'000)) * 100'000'000;
        m::Quantity quantity;
        m::Price at;
        REQUIRE(m::Quantity::from_raw(q, 3, quantity) == Status::Ok);
        REQUIRE(m::Price::from_raw(price, 1, at) == Status::Ok);
        std::int64_t r = 0;
        REQUIRE(p.apply(side, quantity, at, kRaw, cid("C-1"), UnixNanos{1}, r) == Status::Ok);
        realized += r;
        closes += closable != 0 ? 1 : 0;
        (side == m::OrderSide::Buy ? bought : sold) +=
            static_cast<jarvis::core::i128>(price) * static_cast<jarvis::core::i128>(q);
      }
      // Close whatever is left at 100.0.
      if (p.is_open()) {
        const m::OrderSide side = p.signed_raw() > 0 ? m::OrderSide::Sell : m::OrderSide::Buy;
        m::Quantity rest;
        REQUIRE(m::Quantity::from_raw(p.quantity_raw(), 3, rest) == Status::Ok);
        std::int64_t r = 0;
        REQUIRE(p.apply(side, rest, px("100.0"), kRaw, cid("C-1"), UnixNanos{1}, r) == Status::Ok);
        realized += r;
        ++closes;
        (side == m::OrderSide::Buy ? bought : sold) += static_cast<jarvis::core::i128>(100 * kRaw) *
                                                       static_cast<jarvis::core::i128>(rest.raw());
      }
      const jarvis::core::i128 expected = (sold - bought) / (static_cast<jarvis::core::i128>(kRaw));
      const jarvis::core::i128 diff = expected - realized;
      CHECK((diff < 0 ? -diff : diff) <= closes); // one truncation per close at most
    });
  }

  TEST_CASE("the venue position is the sum of the strategies' positions") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      Book b;
      for (int i = 0; i < 60; ++i) {
        const auto strategy = static_cast<std::uint16_t>(gen.below(2));
        const m::OrderSide side = gen.coin() ? m::OrderSide::Buy : m::OrderSide::Sell;
        m::Quantity q;
        m::Price price;
        REQUIRE(m::Quantity::from_raw(gen.range_u(1, 3000) * 1'000'000, 3, q) == Status::Ok);
        REQUIRE(m::Price::from_raw(static_cast<std::int64_t>(gen.range_u(900, 1100)) * 100'000'000,
                                   1, price) == Status::Ok);
        pf::FillOutcome out;
        REQUIRE(b.portfolio.on_fill(b.instrument, 0, strategy, side, q, price, std::nullopt,
                                    cid("C-1"), UnixNanos{10}, out) == Status::Ok);
        REQUIRE(out.booked);
        CHECK(b.portfolio.venue(0).signed_raw() ==
              b.portfolio.position(0, 0).signed_raw() + b.portfolio.position(1, 0).signed_raw());
      }
    });
  }
}
