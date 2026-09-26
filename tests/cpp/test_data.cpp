#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/data/bars.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/data/features.hpp"
#include "jarvis/data/router.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/testkit/alloc.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace d = jarvis::data;
namespace m = jarvis::model;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::testkit::AllocationScope;
using jarvis::testkit::Gen;

m::Price px(std::string_view text) {
  m::Price p;
  REQUIRE(m::Price::parse(text, p) == Status::Ok);
  return p;
}
m::Price px_raw(std::int64_t raw, std::uint8_t precision) {
  m::Price p;
  REQUIRE(m::Price::from_raw(raw, precision, p) == Status::Ok);
  return p;
}
m::Quantity qty(std::string_view text) {
  m::Quantity q;
  REQUIRE(m::Quantity::parse(text, q) == Status::Ok);
  return q;
}
m::Quantity qty_raw(std::uint64_t raw, std::uint8_t precision) {
  m::Quantity q;
  REQUIRE(m::Quantity::from_raw(raw, precision, q) == Status::Ok);
  return q;
}
m::InstrumentId btc() {
  m::InstrumentId id;
  REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
}

m::OrderBookDelta delta(m::BookAction action, m::OrderSide side, m::Price price, m::Quantity size) {
  m::OrderBookDelta dl;
  dl.instrument_id = btc();
  dl.action = action;
  dl.order = m::BookOrder{side, price, size, 0};
  return dl;
}

d::BookConfig book_config(std::uint32_t window, std::uint32_t overflow = 4096) {
  d::BookConfig c;
  c.tick = px("0.1");
  c.size_precision = 3;
  c.window_levels = window;
  c.overflow_levels = overflow;
  return c;
}

// Price in tenths (tick 0.1) and size in thousandths (step 0.001).
m::TradeTick trade(std::int64_t price_tenths, std::uint64_t size_thousandths, std::uint64_t ts) {
  m::TradeTick t;
  t.instrument_id = btc();
  t.price = px_raw(price_tenths * 100'000'000, 1);
  t.size = qty_raw(size_thousandths * 1'000'000, 3);
  t.aggressor_side = m::AggressorSide::Buy;
  t.ts_event = UnixNanos{ts};
  t.ts_init = UnixNanos{ts};
  return t;
}

m::QuoteTick quote(std::string_view bid, std::string_view ask, std::string_view bid_size,
                   std::string_view ask_size) {
  m::QuoteTick q;
  q.instrument_id = btc();
  q.bid_price = px(bid);
  q.ask_price = px(ask);
  q.bid_size = qty(bid_size);
  q.ask_size = qty(ask_size);
  return q;
}

m::BarType bar_type(std::string_view text) {
  m::BarType t;
  REQUIRE(m::BarType::parse(text, t) == Status::Ok);
  return t;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("subscriptions keep order, replace in place and respect capacity") {
    d::SubscriptionMatrix matrix{4, 2};
    CHECK(matrix.subscribe(1, d::DataKind::Trade, 7, d::Cadence::every()) == Status::Ok);
    CHECK(matrix.subscribe(1, d::DataKind::Trade, 3, d::Cadence::conflated()) == Status::Ok);
    CHECK(matrix.subscribe(1, d::DataKind::Trade, 9, d::Cadence::every()) ==
          Status::CapacityExceeded);
    CHECK(matrix.subscribe(1, d::DataKind::Trade, 7, d::Cadence::on_batch()) == Status::Ok);
    auto subs = matrix.subscribers(1, d::DataKind::Trade);
    REQUIRE(subs.size() == 2);
    CHECK(subs[0].strategy == 7);
    CHECK(subs[0].cadence == d::Cadence::on_batch());
    CHECK(subs[1].strategy == 3);
    CHECK(matrix.unsubscribe(1, d::DataKind::Trade, 7) == Status::Ok);
    CHECK(matrix.unsubscribe(1, d::DataKind::Trade, 7) == Status::NotFound);
    REQUIRE(matrix.subscribers(1, d::DataKind::Trade).size() == 1);
    CHECK(matrix.subscribers(1, d::DataKind::Quote).empty());
    CHECK(matrix.subscribe(4, d::DataKind::Trade, 1, d::Cadence::every()) ==
          Status::InvalidArgument);
    CHECK(matrix.subscribe(0, d::DataKind::Trade, 1, d::Cadence::sampled_ns(0)) ==
          Status::InvalidArgument);
    CHECK(d::Cadence::sampled_ms(100).period.value() == 100'000'000);
  }

  TEST_CASE("routes follow the event type") {
    const m::Event t{trade(650'000, 10, 1)};
    const auto r = d::route_of(t);
    REQUIRE(r.has_value());
    const d::Route route = r.value_or(d::Route{});
    CHECK(route.kind == d::DataKind::Trade);
    REQUIRE(route.instrument != nullptr);
    CHECK(*route.instrument == btc());
    CHECK_FALSE(d::route_of(m::Event{m::BatchEnd{1, UnixNanos{1}}}).has_value());
  }

  TEST_CASE("an L2 book applies deltas and reports levels from the touch") {
    d::OrderBook book{book_config(64)};
    using A = m::BookAction;
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Buy, px("100.0"), qty("1.000"))) == Status::Ok);
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Buy, px("99.8"), qty("2.000"))) == Status::Ok);
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Sell, px("100.1"), qty("0.500"))) == Status::Ok);
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Sell, px("100.5"), qty("0.700"))) == Status::Ok);
    d::BookLevel best;
    REQUIRE(book.best_bid(best));
    CHECK(best.price == px("100.0"));
    CHECK(best.size.raw() == qty("1").raw());
    REQUIRE(book.best_ask(best));
    CHECK(best.price == px("100.1"));
    std::array<d::BookLevel, 4> levels{};
    CHECK(book.bids(levels) == 2);
    CHECK(levels[1].price == px("99.8"));
    CHECK(book.asks(levels) == 2);
    CHECK(levels[1].price == px("100.5"));
    REQUIRE(book.apply(delta(A::Update, m::OrderSide::Buy, px("100.0"), qty("3.000"))) ==
            Status::Ok);
    CHECK(book.size_at(m::OrderSide::Buy, px("100.0")).raw() == qty("3").raw());
    REQUIRE(book.apply(delta(A::Delete, m::OrderSide::Buy, px("100.0"), qty("0"))) == Status::Ok);
    REQUIRE(book.best_bid(best));
    CHECK(best.price == px("99.8"));
    CHECK(book.apply(delta(A::Add, m::OrderSide::Buy, px("99.85"), qty("1"))) ==
          Status::InvalidArgument);
    REQUIRE(book.apply(delta(A::Clear, m::OrderSide::Buy, px("0"), qty("0"))) == Status::Ok);
    CHECK_FALSE(book.best_bid(best));
    CHECK_FALSE(book.best_ask(best));
  }

  TEST_CASE("far levels overflow and the window follows the price") {
    d::OrderBook book{book_config(64)}; // 64 ticks = 6.4 USDT of dense window
    using A = m::BookAction;
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Buy, px("100.0"), qty("1"))) == Status::Ok);
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Buy, px("50.0"), qty("5"))) ==
            Status::Ok); // overflow
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Sell, px("100.1"), qty("1"))) == Status::Ok);
    std::array<d::BookLevel, 4> levels{};
    REQUIRE(book.bids(levels) == 2);
    CHECK(levels[1].price == px("50.0"));
    // The market moves 50 USDT down: the window recentres and the far level becomes dense.
    REQUIRE(book.apply(delta(A::Delete, m::OrderSide::Buy, px("100.0"), qty("0"))) == Status::Ok);
    REQUIRE(book.apply(delta(A::Delete, m::OrderSide::Sell, px("100.1"), qty("0"))) == Status::Ok);
    REQUIRE(book.apply(delta(A::Add, m::OrderSide::Sell, px("50.2"), qty("2"))) == Status::Ok);
    d::BookLevel best;
    REQUIRE(book.best_bid(best));
    CHECK(best.price == px("50.0"));
    REQUIRE(book.best_ask(best));
    CHECK(best.price == px("50.2"));
  }

  TEST_CASE("an L1 book keeps only the latest quote") {
    d::BookConfig c = book_config(64);
    c.type = m::BookType::L1_MBP;
    d::OrderBook book{c};
    REQUIRE(book.apply(quote("100.0", "100.2", "1", "2")) == Status::Ok);
    REQUIRE(book.apply(quote("100.1", "100.3", "3", "4")) == Status::Ok);
    std::array<d::BookLevel, 4> levels{};
    CHECK(book.bids(levels) == 1);
    CHECK(levels[0].price == px("100.1"));
    CHECK(book.asks(levels) == 1);
    CHECK(levels[0].size.raw() == qty("4").raw());
    CHECK(book.apply(delta(m::BookAction::Add, m::OrderSide::Buy, px("1"), qty("1"))) ==
          Status::InvalidState);
  }

  TEST_CASE("tick bars close every n trades") {
    d::BarAggregator agg;
    REQUIRE(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-3-TICK-LAST-INTERNAL"), 1, 3,
                                     agg) == Status::Ok);
    std::array<m::Bar, 4> out{};
    std::size_t n = 0;
    REQUIRE(agg.on_trade(trade(1000, 1000, 1), out, n) == Status::Ok);
    REQUIRE(agg.on_trade(trade(1010, 2000, 2), out, n) == Status::Ok);
    CHECK(n == 0);
    REQUIRE(agg.on_trade(trade(990, 3000, 3), out, n) == Status::Ok);
    REQUIRE(n == 1);
    CHECK(out[0].open.raw() == 1000 * 100'000'000LL);
    CHECK(out[0].high.raw() == 1010 * 100'000'000LL);
    CHECK(out[0].low.raw() == 990 * 100'000'000LL);
    CHECK(out[0].close.raw() == 990 * 100'000'000LL);
    CHECK(out[0].volume.raw() == 6'000'000'000ULL);
    CHECK(out[0].ts_init == UnixNanos{3});
  }

  TEST_CASE("volume bars split a trade across the threshold") {
    d::BarAggregator agg;
    REQUIRE(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-1-VOLUME-LAST-INTERNAL"), 1, 3,
                                     agg) == Status::Ok);
    std::array<m::Bar, 8> out{};
    std::size_t n = 0;
    REQUIRE(agg.on_trade(trade(1000, 600, 1), out, n) == Status::Ok); // 0.6
    CHECK(n == 0);
    REQUIRE(agg.on_trade(trade(1001, 2900, 2), out, n) ==
            Status::Ok); // 2.9: fills 0.4, 1, 1, leaves 0.5
    REQUIRE(n == 3);
    for (std::size_t i = 0; i < n; ++i) {
      CHECK(out[i].volume.raw() == 1'000'000'000ULL);
    }
  }

  TEST_CASE("time bars close at the boundary, by timer or by a later update") {
    d::BarAggregator agg;
    REQUIRE(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-1-SECOND-LAST-INTERNAL"), 1, 3,
                                     agg) == Status::Ok);
    std::array<m::Bar, 4> out{};
    std::size_t n = 0;
    CHECK_FALSE(agg.next_close().has_value());
    REQUIRE(agg.on_trade(trade(1000, 1000, 1'500'000'000), out, n) == Status::Ok);
    CHECK(agg.next_close() == std::optional<UnixNanos>{UnixNanos{2'000'000'000}});
    m::Bar bar;
    CHECK_FALSE(agg.on_time(UnixNanos{1'999'999'999}, bar));
    CHECK(agg.on_time(UnixNanos{2'000'000'000}, bar));
    CHECK(bar.ts_event == UnixNanos{2'000'000'000});
    CHECK_FALSE(agg.next_close().has_value()); // no update since: no bar
    REQUIRE(agg.on_trade(trade(1000, 1000, 2'100'000'000), out, n) == Status::Ok);
    REQUIRE(agg.on_trade(trade(1002, 1000, 3'000'000'000), out, n) == Status::Ok);
    REQUIRE(n == 1); // the update at 3s closed the [2s, 3s) bar first
    CHECK(out[0].ts_init == UnixNanos{3'000'000'000});
    CHECK(out[0].close.raw() == 1000 * 100'000'000LL);
    CHECK(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL"), 1, 3,
                                   agg) == Status::InvalidArgument);
    CHECK(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-1-RENKO-LAST-INTERNAL"), 1, 3,
                                   agg) == Status::UnsupportedMessage);
  }

  TEST_CASE("features compute exact fixed-point values") {
    d::Feature ema;
    REQUIRE(d::Feature::create({d::FeatureKind::Ema, btc(), 3}, m::InstrumentSlot{0}, ema) ==
            Status::Ok);
    m::Decimal v;
    bool produced = false;
    REQUIRE(ema.on_trade(trade(1000, 1000, 1), v, produced) == Status::Ok);
    CHECK(produced);
    CHECK(v.raw() == 100'000'000'000); // 100.0
    REQUIRE(ema.on_trade(trade(1100, 1000, 2), v, produced) == Status::Ok);
    CHECK(v.raw() == 105'000'000'000); // 100 + (110 - 100) * 2 / 4

    d::Feature vwap;
    REQUIRE(d::Feature::create({d::FeatureKind::Vwap, btc(), 2}, m::InstrumentSlot{0}, vwap) ==
            Status::Ok);
    REQUIRE(vwap.on_trade(trade(1000, 1000, 1), v, produced) == Status::Ok);
    REQUIRE(vwap.on_trade(trade(1100, 3000, 2), v, produced) == Status::Ok);
    CHECK(v.raw() == 107'500'000'000); // (100*1 + 110*3) / 4
    REQUIRE(vwap.on_trade(trade(1200, 1000, 3), v, produced) == Status::Ok);
    CHECK(v.raw() == 112'500'000'000); // window of 2: (110*3 + 120*1) / 4

    d::Feature imbalance;
    REQUIRE(d::Feature::create({d::FeatureKind::Imbalance, btc(), 0}, m::InstrumentSlot{0},
                               imbalance) == Status::Ok);
    REQUIRE(imbalance.on_quote(quote("100.0", "100.2", "3", "1"), v, produced) == Status::Ok);
    CHECK(v.raw() == 500'000'000); // (3 - 1) / 4

    d::Feature micro;
    REQUIRE(d::Feature::create({d::FeatureKind::Microprice, btc(), 0}, m::InstrumentSlot{0},
                               micro) == Status::Ok);
    REQUIRE(micro.on_quote(quote("100.0", "100.2", "3", "1"), v, produced) == Status::Ok);
    CHECK(v.raw() == 100'150'000'000); // 100 + 0.2 * 3/4

    d::Feature rv;
    REQUIRE(d::Feature::create({d::FeatureKind::RealizedVol, btc(), 10}, m::InstrumentSlot{0},
                               rv) == Status::Ok);
    REQUIRE(rv.on_trade(trade(1000, 1000, 1), v, produced) == Status::Ok);
    CHECK_FALSE(produced);
    REQUIRE(rv.on_trade(trade(1010, 1000, 2), v, produced) == Status::Ok); // +1%
    REQUIRE(produced);
    CHECK(v.raw() == 10'000'000); // sqrt(0.01^2) = 0.01

    CHECK(d::Feature::create({d::FeatureKind::Vwap, btc(), 0}, m::InstrumentSlot{0}, vwap) ==
          Status::InvalidArgument);
    d::FeatureGraph graph{2};
    d::FeatureId a = 0;
    d::FeatureId b = 0;
    REQUIRE(graph.declare({d::FeatureKind::Ema, btc(), 3}, m::InstrumentSlot{0}, a) == Status::Ok);
    REQUIRE(graph.declare({d::FeatureKind::Ema, btc(), 3}, m::InstrumentSlot{0}, b) == Status::Ok);
    CHECK(a == b);
    CHECK(graph.size() == 1);
    CHECK(jarvis::core::isqrt(static_cast<jarvis::core::u128>(1'000'000'000'000'000'000ULL) *
                              100U) == 10'000'000'000ULL);
  }
}

TEST_SUITE("property") {
  TEST_CASE("the book agrees with a map reference under random deltas") {
    jarvis::testkit::for_all([](Gen& gen) {
      d::OrderBook book{book_config(64, 4096)};
      std::map<std::int64_t, std::uint64_t> bids;
      std::map<std::int64_t, std::uint64_t> asks;
      std::int64_t centre = 10'000 + static_cast<std::int64_t>(gen.below(1000));
      for (int i = 0; i < 300; ++i) {
        centre += static_cast<std::int64_t>(gen.below(41)) - 20; // drift, forcing recentres
        const bool bid = gen.coin();
        const std::int64_t tick =
            centre + (bid ? -1 : 1) * static_cast<std::int64_t>(gen.below(200));
        const bool remove = gen.chance(1, 4);
        const std::uint64_t size = remove ? 0 : (1 + gen.below(5000)) * 1'000'000;
        auto& side = bid ? bids : asks;
        const m::BookAction action = remove ? m::BookAction::Delete : m::BookAction::Update;
        REQUIRE(book.apply(delta(action, bid ? m::OrderSide::Buy : m::OrderSide::Sell,
                                 px_raw(tick * 100'000'000, 1), qty_raw(size, 3))) == Status::Ok);
        if (remove) {
          side.erase(tick);
        } else {
          side[tick] = size;
        }
      }
      std::array<d::BookLevel, 512> levels{};
      const std::size_t nb = book.bids(levels);
      REQUIRE(nb == bids.size());
      std::size_t i = 0;
      for (auto it = bids.rbegin(); it != bids.rend(); ++it, ++i) {
        CHECK(levels[i].price.raw() == it->first * 100'000'000);
        CHECK(levels[i].size.raw() == it->second);
      }
      const std::size_t na = book.asks(levels);
      REQUIRE(na == asks.size());
      i = 0;
      for (auto it = asks.begin(); it != asks.end(); ++it, ++i) {
        CHECK(levels[i].price.raw() == it->first * 100'000'000);
      }
    });
  }

  TEST_CASE("volume bars conserve volume") {
    jarvis::testkit::for_all([](Gen& gen) {
      d::BarAggregator agg;
      REQUIRE(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-2-VOLUME-LAST-INTERNAL"), 1,
                                       3, agg) == Status::Ok);
      std::uint64_t traded = 0;
      std::uint64_t barred = 0;
      std::array<m::Bar, 16> out{};
      for (int i = 0; i < 200; ++i) {
        const std::uint64_t size = (1 + gen.below(5000)) * 1'000'000; // up to 5 units
        traded += size;
        std::size_t n = 0;
        REQUIRE(agg.on_trade(trade(1000 + static_cast<std::int64_t>(gen.below(10)),
                                   size / 1'000'000, static_cast<std::uint64_t>(i)),
                             out, n) == Status::Ok);
        for (std::size_t k = 0; k < n; ++k) {
          CHECK(out[k].volume.raw() == 2'000'000'000ULL);
          CHECK(out[k].low <= out[k].open);
          CHECK(out[k].high >= out[k].close);
          barred += out[k].volume.raw();
        }
      }
      CHECK(traded - barred < 2'000'000'000ULL);
    });
  }

  TEST_CASE("VWAP stays inside the window's price range") {
    jarvis::testkit::for_all([](Gen& gen) {
      const auto window = static_cast<std::uint32_t>(gen.range(1, 20));
      d::Feature vwap;
      REQUIRE(d::Feature::create({d::FeatureKind::Vwap, btc(), window}, m::InstrumentSlot{0},
                                 vwap) == Status::Ok);
      std::vector<std::int64_t> prices;
      for (int i = 0; i < 100; ++i) {
        const std::int64_t p = 1000 + static_cast<std::int64_t>(gen.below(500));
        prices.push_back(p);
        m::Decimal v;
        bool produced = false;
        REQUIRE(vwap.on_trade(trade(p, 1 + gen.below(1000), static_cast<std::uint64_t>(i)), v,
                              produced) == Status::Ok);
        const std::size_t from = prices.size() > window ? prices.size() - window : 0;
        std::int64_t lo = INT64_MAX;
        std::int64_t hi = INT64_MIN;
        for (std::size_t k = from; k < prices.size(); ++k) {
          lo = std::min(lo, prices[k] * 100'000'000);
          hi = std::max(hi, prices[k] * 100'000'000);
        }
        CHECK(v.raw() >= lo);
        CHECK(v.raw() <= hi);
      }
    });
  }
}

TEST_SUITE("zero-alloc") {
  TEST_CASE("book updates, bars and features do not allocate") {
    d::OrderBook book{book_config(1024)};
    d::BarAggregator agg;
    REQUIRE(d::BarAggregator::create(bar_type("BTCUSDT-PERP.BINANCE-5-TICK-LAST-INTERNAL"), 1, 3,
                                     agg) == Status::Ok);
    d::FeatureGraph graph{4};
    d::FeatureId id = 0;
    REQUIRE(graph.declare({d::FeatureKind::Vwap, btc(), 16}, m::InstrumentSlot{0}, id) ==
            Status::Ok);
    std::array<m::Bar, 4> bars{};
    std::array<d::BookLevel, 8> levels{};
    const AllocationScope scope;
    for (int i = 0; i < 1000; ++i) {
      const std::int64_t tick = 10'000 + (i * 37) % 3000;
      static_cast<void>(book.apply(delta(m::BookAction::Update,
                                         i % 2 == 0 ? m::OrderSide::Buy : m::OrderSide::Sell,
                                         px_raw(tick * 100'000'000, 1), qty_raw(1'000'000, 3))));
      static_cast<void>(book.bids(levels));
      std::size_t n = 0;
      const m::TradeTick t = trade(tick, 1000, static_cast<std::uint64_t>(i));
      static_cast<void>(agg.on_trade(t, bars, n));
      m::Decimal v;
      bool produced = false;
      static_cast<void>(graph.at(id).on_trade(t, v, produced));
    }
    CHECK(scope.allocations() == 0);
  }
}
