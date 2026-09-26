// A node for the golden replay cases (tests/golden/replay_*): one strategy whose `plan`
// parameter picks what it subscribes to, and a deterministic data catalog to run it on.
//
//   golden_node catalog --seed N --out DIR     writes the catalog (one day, BTCUSDT-PERP)
//   golden_node --config FILE ...              node_main<GoldenStrategy> (run and replay)
//
// Plans: trade (every trade, and trades per batch), quote (conflated and sampled quotes),
// book (L2 deltas and the conflated book), bar (external klines, internal time and tick bars),
// feature (EMA, VWAP, imbalance, microprice, realized volatility).

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/data/book.hpp"
#include "jarvis/data/features.hpp"
#include "jarvis/data/subscription.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/log_source.hpp"
#include "jarvis/node/node_main.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"

namespace {

namespace m = jarvis::model;
namespace d = jarvis::data;
namespace st = jarvis::strategy;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

constexpr std::uint64_t kDay = 1'788'220'800'000'000'000ULL; // 2026-09-01T00:00:00Z
constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::string_view kInstrument = "BTCUSDT-PERP.BINANCE";

m::InstrumentId btc() {
  m::InstrumentId id;
  static_cast<void>(m::InstrumentId::parse(kInstrument, id));
  return id;
}

m::Price px(std::int64_t tenths) {
  m::Price p;
  static_cast<void>(m::Price::from_raw(tenths * 100'000'000, 1, p));
  return p;
}

m::Quantity qty(std::uint64_t thousandths) {
  m::Quantity q;
  static_cast<void>(m::Quantity::from_raw(thousandths * 1'000'000, 3, q));
  return q;
}

class SplitMix {
public:
  explicit SplitMix(std::uint64_t seed) : state_{seed} {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }

private:
  std::uint64_t state_;
};

bool open_log(jarvis::node::EventLogWriter& w, const std::string& catalog,
              std::string_view stream) {
  const std::string dir =
      jarvis::node::catalog_directory(catalog, btc(), std::string{stream}, "2026-09-01");
  return jarvis::core::ok(w.open(dir, m::wire::LogHeader{}, {}));
}

// Ten minutes of market data from 00:00 with a random walk mid.
int write_catalog(std::uint64_t seed, const std::string& out) {
  SplitMix rng{seed};
  jarvis::node::EventLogWriter trades;
  jarvis::node::EventLogWriter quotes;
  jarvis::node::EventLogWriter depth;
  jarvis::node::EventLogWriter klines;
  if (!open_log(trades, out, "aggTrade") || !open_log(quotes, out, "bookTicker") ||
      !open_log(depth, out, "depth") || !open_log(klines, out, "kline_1m")) {
    std::fprintf(stderr, "golden_node: cannot create the catalog in %s\n", out.c_str());
    return 1;
  }
  std::int64_t mid = 650'000; // tenths
  std::uint64_t trade_seq = 0;
  std::uint64_t quote_seq = 0;
  std::uint64_t depth_seq = 0;
  std::vector<m::OrderBookDelta> batch;
  // A snapshot of five levels per side, then updates.
  for (std::int64_t level = 1; level <= 5; ++level) {
    for (const bool bid : {true, false}) {
      m::OrderBookDelta dl;
      dl.instrument_id = btc();
      dl.action = m::BookAction::Add;
      dl.order.side = bid ? m::OrderSide::Buy : m::OrderSide::Sell;
      dl.order.price = px(bid ? mid - level : mid + level);
      dl.order.size = qty(1000 + static_cast<std::uint64_t>(level) * 100);
      dl.flags = m::flag_bit(m::RecordFlag::F_SNAPSHOT);
      dl.sequence = 1;
      dl.ts_event = UnixNanos{kDay + 1};
      dl.ts_init = UnixNanos{kDay + 1};
      batch.push_back(dl);
    }
  }
  batch.back().flags |= m::flag_bit(m::RecordFlag::F_LAST);
  m::OrderBookDeltas snapshot;
  static_cast<void>(m::OrderBookDeltas::create(batch, snapshot));
  bool good = jarvis::core::ok(
      depth.append(EventKey{UnixNanos{kDay + 1}, 3, ++depth_seq}, m::Event{snapshot}));
  for (std::uint64_t step = 1; step <= 600 && good; ++step) { // one step per second
    const std::uint64_t ts = kDay + step * kSecond;
    mid += static_cast<std::int64_t>(rng.below(5)) - 2;
    if (step % 60 == 0) { // the kline that closed at this second
      m::Bar bar;
      m::BarType type;
      static_cast<void>(m::BarType::parse("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL", type));
      static_cast<void>(m::Bar::create(type, px(mid - 3), px(mid + 4), px(mid - 5), px(mid),
                                       qty(12'345), UnixNanos{ts}, UnixNanos{ts}, bar));
      good = good &&
             jarvis::core::ok(klines.append(EventKey{UnixNanos{ts}, 4, step / 60}, m::Event{bar}));
    }
    if (step % 2 == 0) {
      // One draw per statement: the order of evaluation of function arguments is unspecified,
      // and the catalog must be the same whatever the compiler.
      const std::uint64_t bid_size = 500 + rng.below(1500);
      const std::uint64_t ask_size = 500 + rng.below(1500);
      m::QuoteTick q;
      static_cast<void>(m::QuoteTick::create(btc(), px(mid - 1), px(mid + 1), qty(bid_size),
                                             qty(ask_size), UnixNanos{ts}, UnixNanos{ts}, q));
      good = good &&
             jarvis::core::ok(quotes.append(EventKey{UnixNanos{ts}, 2, ++quote_seq}, m::Event{q}));
    }
    for (std::uint64_t k = rng.below(3); k > 0; --k) { // 0-2 trades at this second
      const auto offset = static_cast<std::int64_t>(rng.below(3)) - 1;
      const std::uint64_t size = 1 + rng.below(900);
      const bool buy = rng.below(2) == 0;
      m::TradeTick t;
      m::TradeId id;
      static_cast<void>(m::TradeId::from(std::to_string(trade_seq + 1), id));
      static_cast<void>(m::TradeTick::create(btc(), px(mid + offset), qty(size),
                                             buy ? m::AggressorSide::Buy : m::AggressorSide::Sell,
                                             id, UnixNanos{ts}, UnixNanos{ts}, t));
      good = good &&
             jarvis::core::ok(trades.append(EventKey{UnixNanos{ts}, 1, ++trade_seq}, m::Event{t}));
    }
    if (step % 5 == 0) { // an L2 update on both sides
      batch.clear();
      for (const bool bid : {true, false}) {
        m::OrderBookDelta dl;
        dl.instrument_id = btc();
        dl.action = m::BookAction::Update;
        dl.order.side = bid ? m::OrderSide::Buy : m::OrderSide::Sell;
        dl.order.price = px(bid ? mid - 1 : mid + 1);
        dl.order.size = qty(100 + rng.below(2000));
        dl.sequence = 1 + step;
        dl.ts_event = UnixNanos{ts};
        dl.ts_init = UnixNanos{ts};
        batch.push_back(dl);
      }
      batch.back().flags = m::flag_bit(m::RecordFlag::F_LAST);
      m::OrderBookDeltas deltas;
      static_cast<void>(m::OrderBookDeltas::create(batch, deltas));
      good = good && jarvis::core::ok(
                         depth.append(EventKey{UnixNanos{ts}, 3, ++depth_seq}, m::Event{deltas}));
    }
  }
  good = good && jarvis::core::ok(trades.close()) && jarvis::core::ok(quotes.close()) &&
         jarvis::core::ok(depth.close()) && jarvis::core::ok(klines.close());
  if (!good) {
    std::fprintf(stderr, "golden_node: writing the catalog failed\n");
    return 1;
  }
  return 0;
}

m::Decimal decimal9(std::int64_t raw) {
  m::Decimal v;
  static_cast<void>(m::Decimal::from_raw(raw, 9, v));
  return v;
}

m::Decimal count(std::uint64_t n) {
  m::Decimal v;
  static_cast<void>(m::Decimal::from_raw(static_cast<std::int64_t>(n) * 1'000'000'000, 0, v));
  return v;
}

struct GoldenStrategy {
  std::string plan;

  static Status create(const jarvis::node::StrategyParams& p, GoldenStrategy& out) {
    std::string_view plan;
    const Status s = p.get("plan", plan);
    out.plan = std::string{plan};
    return s;
  }

  [[nodiscard]] Status on_start(st::Context& ctx) const {
    const m::InstrumentId id = btc();
    if (plan == "trade") {
      return ctx.subscribe_trades(id);
    }
    if (plan == "batch") {
      return ctx.subscribe_trades(id, d::Cadence::on_batch());
    }
    if (plan == "quote") {
      Status s = ctx.subscribe_quotes(id, d::Cadence::conflated());
      return s;
    }
    if (plan == "book") {
      Status s = ctx.subscribe_book_deltas(id);
      return jarvis::core::ok(s) ? ctx.subscribe_book(id, d::Cadence::sampled_ms(10'000)) : s;
    }
    if (plan == "bar") {
      Status s = Status::Ok;
      for (const std::string_view text : {"BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL",
                                          "BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-INTERNAL",
                                          "BTCUSDT-PERP.BINANCE-10-TICK-LAST-INTERNAL",
                                          "BTCUSDT-PERP.BINANCE-30-SECOND-MID-INTERNAL"}) {
        m::BarType type;
        s = jarvis::core::ok(s) ? m::BarType::parse(text, type) : s;
        s = jarvis::core::ok(s) ? ctx.subscribe_bars(type) : s;
      }
      return s;
    }
    if (plan == "feature") {
      const std::array<std::pair<d::FeatureSpec, d::Cadence>, 5> specs = {{
          {{d::FeatureKind::Ema, id, 10}, d::Cadence::every()},
          {{d::FeatureKind::Vwap, id, 20}, d::Cadence::sampled_ms(30'000)},
          {{d::FeatureKind::Imbalance, id, 0}, d::Cadence::conflated()},
          {{d::FeatureKind::Microprice, id, 0}, d::Cadence::sampled_ms(60'000)},
          {{d::FeatureKind::RealizedVol, id, 20}, d::Cadence::sampled_ms(60'000)},
      }};
      for (const auto& [spec, cadence] : specs) {
        m::FeatureId fid = 0;
        const Status s = ctx.feature(spec, cadence, fid);
        if (!jarvis::core::ok(s)) {
          return s;
        }
      }
      return Status::Ok;
    }
    return Status::InvalidArgument;
  }

  static Status on_trade(st::Context& ctx, const m::TradeTick& t) {
    return ctx.record("trade", decimal9(t.price.raw()));
  }
  static Status on_trade_batch(st::Context& ctx, const st::TradeBatch& b) {
    return ctx.record("batch", count(b.trades.size()));
  }
  static Status on_quote(st::Context& ctx, const m::QuoteTick& q) {
    return ctx.record("mid", decimal9((q.bid_price.raw() + q.ask_price.raw()) / 2));
  }
  static Status on_book_deltas(st::Context& ctx, const m::OrderBookDeltas& deltas) {
    return ctx.record("deltas", count(deltas.deltas.size()));
  }
  static Status on_book(st::Context& ctx, const d::BookView& book) {
    d::BookLevel bid;
    d::BookLevel ask;
    if (!book.best_bid(bid) || !book.best_ask(ask)) {
      return Status::Ok;
    }
    Status s = ctx.record("bid", decimal9(bid.price.raw()));
    return jarvis::core::ok(s) ? ctx.record("ask", decimal9(ask.price.raw())) : s;
  }
  static Status on_bar(st::Context& ctx, const m::Bar& bar) {
    return ctx.record(bar.bar_type.spec.aggregation == m::BarAggregation::Tick ? "tick_bar" : "bar",
                      decimal9(bar.close.raw()));
  }
};

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc); // NOLINT
  if (args.size() == 6 && args[1] == "catalog" && args[2] == "--seed" && args[4] == "--out") {
    std::uint64_t seed = 0;
    const auto [end, ec] = std::from_chars(args[3].data(), args[3].data() + args[3].size(), seed);
    if (ec != std::errc{} || end != args[3].data() + args[3].size()) {
      std::fprintf(stderr, "golden_node: bad seed\n");
      return 2;
    }
    return write_catalog(seed, std::string{args[5]});
  }
  return jarvis::node_main<GoldenStrategy>(argc, argv);
}
