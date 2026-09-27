#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/backtest/driver.hpp"
#include "jarvis/backtest/matching/sim_exchange.hpp"
#include "jarvis/backtest/venue_loop.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/cost/fees.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace bt = jarvis::backtest;
namespace ex = jarvis::execution;
namespace st = jarvis::strategy;
namespace m = jarvis::model;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

m::InstrumentId btc() {
  m::InstrumentId id;
  REQUIRE(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id) == Status::Ok);
  return id;
}
m::Price px(std::string_view t) {
  m::Price p;
  REQUIRE(m::Price::parse(t, p) == Status::Ok);
  return p;
}
m::Quantity qty(std::string_view t) {
  m::Quantity q;
  REQUIRE(m::Quantity::parse(t, q) == Status::Ok);
  return q;
}
m::ClientOrderId cid(std::string_view t) {
  m::ClientOrderId id;
  REQUIRE(m::ClientOrderId::from(t, id) == Status::Ok);
  return id;
}

m::CryptoPerpetual perpetual() {
  m::CryptoPerpetual p;
  m::InstrumentCommon& c = p.common;
  c.id = btc();
  REQUIRE(m::Symbol::from("BTCUSDT", c.raw_symbol) == Status::Ok);
  REQUIRE(m::Currency::builtin("BTC", c.base_currency.emplace()) == Status::Ok);
  REQUIRE(m::Currency::builtin("USDT", c.quote_currency) == Status::Ok);
  c.settlement_currency = c.quote_currency;
  c.price_precision = 1;
  c.size_precision = 3;
  c.price_increment = px("0.1");
  c.size_increment = qty("0.001");
  c.multiplier = qty("1");
  REQUIRE(m::Decimal::parse("0.05", c.margin_init) == Status::Ok);
  REQUIRE(m::Decimal::parse("0.025", c.margin_maint) == Status::Ok);
  return p;
}

m::Event quote(std::uint64_t ts, std::string_view bid, std::string_view ask,
               std::string_view bid_size = "1.000", std::string_view ask_size = "2.000") {
  m::QuoteTick q;
  q.instrument_id = btc();
  q.bid_price = px(bid);
  q.ask_price = px(ask);
  q.bid_size = qty(bid_size);
  q.ask_size = qty(ask_size);
  q.ts_event = UnixNanos{ts};
  q.ts_init = UnixNanos{ts};
  return m::Event{q};
}

m::Event trade(std::uint64_t ts, std::string_view price, std::string_view size,
               m::AggressorSide aggressor) {
  m::TradeTick t;
  t.instrument_id = btc();
  t.price = px(price);
  t.size = qty(size);
  t.aggressor_side = aggressor;
  REQUIRE(m::TradeId::from(std::to_string(ts), t.trade_id) == Status::Ok);
  t.ts_event = UnixNanos{ts};
  t.ts_init = UnixNanos{ts};
  return m::Event{t};
}

m::Output submit(std::string_view id, m::OrderSide side, std::string_view q,
                 std::optional<std::string_view> price, m::TimeInForce tif = m::TimeInForce::Gtc,
                 bool post_only = false, bool reduce_only = false) {
  m::SubmitOrder s;
  s.client_order_id = cid(id);
  s.instrument_id = btc();
  s.order_side = side;
  s.order_type = price ? m::OrderType::Limit : m::OrderType::Market;
  s.quantity = qty(q);
  if (price) {
    s.price = px(*price);
  }
  s.time_in_force = price ? tif : m::TimeInForce::Ioc;
  s.post_only = post_only;
  s.reduce_only = reduce_only;
  return m::Output{s};
}

std::string line(const m::OrderEvent& e) {
  ex::OrderEventKind k{};
  REQUIRE(ex::kind_of(e, k));
  std::string out{ex::to_string(k)};
  out += " " + std::string{m::header_of(e).client_order_id.view()};
  if (const auto* f = std::get_if<m::OrderFilled>(&e)) {
    std::array<char, 64> b{};
    std::size_t n = 0;
    REQUIRE(f->last_qty.format(b, n) == Status::Ok);
    out += " " + std::string{b.data(), n};
    REQUIRE(f->last_px.format(b, n) == Status::Ok);
    out += "@" + std::string{b.data(), n};
    out += f->liquidity_side == m::LiquiditySide::Maker ? " M" : " T";
  } else if (const auto* r = std::get_if<m::OrderRejected>(&e)) {
    out += " " + std::string{r->reason.view()};
  }
  return out;
}

struct Venue {
  bt::SimulatedExchange sim;
  std::uint64_t ts = 100;

  explicit Venue(bt::FillModel model = bt::FillModel::TopOfBook,
                 bt::StpMode stp = bt::StpMode::None)
      : sim{config(model, stp)} {
    REQUIRE(sim.on_data(m::Event{perpetual()}, UnixNanos{1}) == Status::Ok);
  }
  static bt::SimConfig config(bt::FillModel model, bt::StpMode stp) {
    bt::SimConfig c;
    c.fill_model = model;
    c.stp = stp;
    c.instruments = 4;
    c.orders = 64;
    c.book_levels = 1024;
    REQUIRE(jarvis::cost::MakerTakerFees::schedule("binance_usdm_vip0", c.fees) == Status::Ok);
    return c;
  }
  std::vector<std::string> data(const m::Event& e) {
    sim.clear_events();
    REQUIRE(sim.on_data(e, UnixNanos{++ts}) == Status::Ok);
    return lines();
  }
  std::vector<std::string> command(const m::Output& o) {
    sim.clear_events();
    REQUIRE(sim.on_command(o, UnixNanos{++ts}) == Status::Ok);
    return lines();
  }
  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    for (const m::OrderEvent& e : sim.events()) {
      out.push_back(line(e));
    }
    return out;
  }
};

using Lines = std::vector<std::string>;

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("a crossing limit order takes the top of book, capped by its size; the rest rests") {
    Venue v;
    v.data(quote(0, "100.0", "100.1"));
    CHECK(v.command(submit("A", m::OrderSide::Buy, "3.000", "100.2")) ==
          Lines{"ACCEPTED A", "FILLED A 2.000@100.1 T"});
    std::array<bt::SimOrder, 4> open{};
    REQUIRE(v.sim.open_orders(open) == 1);
    CHECK(open[0].leaves_raw() == qty("1.000").raw());
    // A later quote that offers at the resting price fills it as a maker, at its own price.
    CHECK(v.data(quote(0, "100.0", "100.3")) == Lines{});
    CHECK(v.data(quote(0, "100.1", "100.2", "1.000", "0.400")) ==
          Lines{"FILLED A 0.400@100.2 M"}); // up to the size offered
    CHECK(v.data(quote(0, "100.1", "100.2")) == Lines{"FILLED A 0.600@100.2 M"});
    CHECK(v.sim.stats().maker_fills == 2);
    CHECK(v.sim.stats().taker_fills == 1);
  }

  TEST_CASE("IOC and market remainders expire; FOK fills completely or expires unaccepted") {
    Venue v;
    v.data(quote(0, "100.0", "100.1"));
    CHECK(v.command(submit("I", m::OrderSide::Buy, "3.000", "100.1", m::TimeInForce::Ioc)) ==
          Lines{"ACCEPTED I", "FILLED I 2.000@100.1 T", "EXPIRED I"});
    CHECK(v.command(submit("F", m::OrderSide::Buy, "3.000", "100.1", m::TimeInForce::Fok)) ==
          Lines{"EXPIRED F"});
    CHECK(v.command(submit("G", m::OrderSide::Buy, "2.000", "100.1", m::TimeInForce::Fok)) ==
          Lines{"ACCEPTED G", "FILLED G 2.000@100.1 T"});
    CHECK(v.command(submit("M", m::OrderSide::Sell, "1.500", std::nullopt)) ==
          Lines{"ACCEPTED M", "FILLED M 1.000@100.0 T", "EXPIRED M"});
    Venue empty;
    CHECK(empty.command(submit("N", m::OrderSide::Sell, "1.000", std::nullopt)) ==
          Lines{"REJECTED N NO_MARKET"});
  }

  TEST_CASE("post-only orders that would take are rejected; others rest") {
    Venue v;
    v.data(quote(0, "100.0", "100.1"));
    CHECK(v.command(submit("P", m::OrderSide::Buy, "1.000", "100.1", m::TimeInForce::Gtc, true)) ==
          Lines{"REJECTED P -5022 POST_ONLY_WOULD_TAKE"});
    CHECK(v.command(submit("Q", m::OrderSide::Buy, "1.000", "100.0", m::TimeInForce::Gtc, true)) ==
          Lines{"ACCEPTED Q"});
  }

  TEST_CASE("top of book: trades through the price fill, trades at it do not") {
    Venue v;
    v.data(quote(0, "100.0", "100.1"));
    v.command(submit("B", m::OrderSide::Buy, "2.000", "100.0"));
    CHECK(v.data(trade(0, "100.0", "5.000", m::AggressorSide::Sell)) == Lines{});
    CHECK(v.data(trade(0, "100.1", "5.000", m::AggressorSide::Sell)) == Lines{}); // above
    CHECK(v.data(trade(0, "99.9", "0.500", m::AggressorSide::Buy)) == Lines{});   // buyers
    CHECK(v.data(trade(0, "99.9", "0.500", m::AggressorSide::Sell)) ==
          Lines{"FILLED B 0.500@100.0 M"});
    CHECK(v.data(quote(0, "99.8", "100.0", "1.000", "3.000")) == Lines{"FILLED B 1.500@100.0 M"});
  }

  TEST_CASE("queue position: trades at the price consume the queue ahead first") {
    Venue v{bt::FillModel::QueuePosition};
    v.data(quote(0, "100.0", "100.1", "3.000", "2.000"));
    v.command(submit("B", m::OrderSide::Buy, "2.000", "100.0")); // 3.000 ahead
    CHECK(v.data(trade(0, "100.0", "2.500", m::AggressorSide::Sell)) == Lines{});
    CHECK(v.data(trade(0, "100.0", "1.000", m::AggressorSide::Sell)) ==
          Lines{"FILLED B 0.500@100.0 M"});
    // Cancellations ahead shrink the queue in proportion; here nothing is left ahead.
    CHECK(v.data(trade(0, "100.0", "2.000", m::AggressorSide::Sell)) ==
          Lines{"FILLED B 1.500@100.0 M"});

    Venue w{bt::FillModel::QueuePosition};
    w.data(quote(0, "100.0", "100.1", "4.000", "2.000"));
    w.command(submit("C", m::OrderSide::Buy, "1.000", "100.0")); // 4.000 ahead
    w.data(quote(0, "100.0", "100.1", "2.000", "2.000"));        // half of the level left
    CHECK(w.data(trade(0, "100.0", "2.500", m::AggressorSide::Sell)) ==
          Lines{"FILLED C 0.500@100.0 M"});              // 2.000 ahead, 0.500 for the order
    w.data(quote(0, "99.9", "100.1", "1.000", "2.000")); // the level is gone: nothing ahead
    CHECK(w.data(trade(0, "100.0", "0.100", m::AggressorSide::Sell)) ==
          Lines{"FILLED C 0.100@100.0 M"});
  }

  TEST_CASE("an L2 book: market orders walk the levels, resting orders fill when crossed") {
    Venue v;
    std::array<m::OrderBookDelta, 3> storage{};
    const auto level = [](m::OrderSide side, std::string_view price, std::string_view size,
                          std::uint8_t flags) {
      m::OrderBookDelta d;
      d.instrument_id = btc();
      d.action = m::BookAction::Update;
      d.order.side = side;
      d.order.price = px(price);
      d.order.size = qty(size);
      d.flags = flags;
      return d;
    };
    storage = {level(m::OrderSide::Sell, "100.1", "1.000", 0),
               level(m::OrderSide::Sell, "100.2", "1.000", 0),
               level(m::OrderSide::Buy, "100.0", "1.000", 128)};
    m::OrderBookDeltas deltas;
    REQUIRE(m::OrderBookDeltas::create(storage, deltas) == Status::Ok);
    v.data(m::Event{deltas});
    CHECK(v.command(submit("M", m::OrderSide::Buy, "1.500", std::nullopt)) ==
          Lines{"ACCEPTED M", "FILLED M 1.000@100.1 T", "FILLED M 0.500@100.2 T"});
    v.command(submit("R", m::OrderSide::Sell, "0.300", "100.3"));
    storage = {level(m::OrderSide::Buy, "100.3", "0.200", 0),
               level(m::OrderSide::Buy, "100.4", "0.050", 0),
               level(m::OrderSide::Sell, "100.5", "1.000", 128)};
    REQUIRE(m::OrderBookDeltas::create(storage, deltas) == Status::Ok);
    CHECK(v.data(m::Event{deltas}) == Lines{"FILLED R 0.250@100.3 M"});
  }

  TEST_CASE("a proportional queue shrink stays on the lot grid, so fills do too") {
    Venue v{bt::FillModel::QueuePosition};
    v.data(quote(0, "100.0", "100.1", "0.003", "1.000"));
    v.command(submit("A", m::OrderSide::Buy, "0.005", "100.0")); // 0.003 ahead
    std::array<bt::SimOrder, 2> open{};
    // Growth joins behind (0.003 ahead of 0.005); a fall to 0.004 leaves 0.003 x 4/5 = 0.0024
    // ahead, rounded down to the lot: 0.002.
    v.data(quote(0, "100.0", "100.1", "0.005", "1.000"));
    v.data(quote(0, "100.0", "100.1", "0.004", "1.000"));
    REQUIRE(v.sim.open_orders(open) == 1);
    CHECK(open[0].ahead_raw == qty("0.002").raw());
    // Before the rounding the queue ahead was 0.0024 and this fill 0.0006, off the grid.
    CHECK(v.data(trade(0, "100.0", "0.003", m::AggressorSide::Sell)) ==
          Lines{"FILLED A 0.001@100.0 M"});
  }

  TEST_CASE("cancel, modify and their rejections") {
    Venue v{bt::FillModel::QueuePosition};
    v.data(quote(0, "100.0", "100.1"));
    v.command(submit("A", m::OrderSide::Buy, "1.000", "99.9"));
    m::ModifyOrder modify;
    modify.client_order_id = cid("A");
    modify.instrument_id = btc();
    modify.quantity = qty("2.000");
    modify.price = px("99.8");
    CHECK(v.command(m::Output{modify}) == Lines{"UPDATED A"});
    modify.price = px("100.1");
    CHECK(v.command(m::Output{modify}) == Lines{"UPDATED A", "FILLED A 2.000@100.1 T"});
    modify.client_order_id = cid("Z");
    CHECK(v.command(m::Output{modify}) == Lines{"MODIFY_REJECTED Z"});
    v.command(submit("C", m::OrderSide::Sell, "1.000", "101.0"));
    m::CancelOrder cancel;
    cancel.client_order_id = cid("C");
    cancel.instrument_id = btc();
    CHECK(v.command(m::Output{cancel}) == Lines{"CANCELED C"});
    CHECK(v.command(m::Output{cancel}) == Lines{"CANCEL_REJECTED C"});
    v.command(submit("D", m::OrderSide::Sell, "1.000", "101.0"));
    v.command(submit("E", m::OrderSide::Sell, "1.000", "102.0"));
    m::CancelAllOrders all;
    all.instrument_id = btc();
    CHECK(v.command(m::Output{all}) == Lines{"CANCELED D", "CANCELED E"});
  }

  TEST_CASE("reduce-only needs a position to reduce; the venue tracks its own") {
    Venue v;
    v.data(quote(0, "100.0", "100.1"));
    CHECK(v.command(submit("R", m::OrderSide::Sell, "1.000", std::nullopt, m::TimeInForce::Ioc,
                           false, true)) == Lines{"REJECTED R -2022 REDUCE_ONLY_REJECTED"});
    v.command(submit("B", m::OrderSide::Buy, "1.000", std::nullopt));
    CHECK(v.sim.account().venue(0).signed_raw() == 1'000'000'000);
    CHECK(v.command(submit("S", m::OrderSide::Sell, "1.000", std::nullopt, m::TimeInForce::Ioc,
                           false, true)) == Lines{"ACCEPTED S", "FILLED S 1.000@100.0 T"});
  }

  TEST_CASE("self-trade prevention") {
    Venue taker{bt::FillModel::TopOfBook, bt::StpMode::ExpireTaker};
    taker.data(quote(0, "100.0", "100.2"));
    taker.command(submit("S", m::OrderSide::Sell, "1.000", "100.1"));
    CHECK(taker.command(submit("B", m::OrderSide::Buy, "1.000", "100.2")) ==
          Lines{"ACCEPTED B", "EXPIRED B"});
    Venue maker{bt::FillModel::TopOfBook, bt::StpMode::ExpireMaker};
    maker.data(quote(0, "100.0", "100.2"));
    maker.command(submit("S", m::OrderSide::Sell, "1.000", "100.1"));
    CHECK(maker.command(submit("B", m::OrderSide::Buy, "1.000", "100.2")) ==
          Lines{"ACCEPTED B", "EXPIRED S", "FILLED B 1.000@100.2 T"});
  }

  TEST_CASE("GTD orders expire at their time; fills pay the fee schedule") {
    Venue v;
    v.data(quote(0, "100.0", "100.1"));
    m::Output gtd = submit("G", m::OrderSide::Buy, "1.000", "99.0", m::TimeInForce::Gtd);
    std::get<m::SubmitOrder>(gtd).expire_time = UnixNanos{v.ts + 3};
    CHECK(v.command(gtd) == Lines{"ACCEPTED G"});
    CHECK(v.data(quote(0, "100.0", "100.1")) == Lines{});
    CHECK(v.data(quote(0, "100.0", "100.1")) == Lines{"EXPIRED G"});
    v.command(submit("T", m::OrderSide::Buy, "1.000", std::nullopt));
    const auto& filled = std::get<m::OrderFilled>(v.sim.events()[1]);
    m::Money expected;
    REQUIRE(m::Money::parse("0.05005 USDT", expected) == Status::Ok); // 100.1 x 0.05%
    CHECK(filled.commission == expected);
  }
}

namespace {

struct Keyed {
  EventKey key;
  m::Event event;
};

class VectorSource {
public:
  explicit VectorSource(std::vector<Keyed> items) : items_{std::move(items)} {}
  Status next(EventKey& key, m::Event& event) {
    if (index_ >= items_.size()) {
      return Status::EndOfStream;
    }
    key = items_[index_].key;
    event = items_[index_].event;
    ++index_;
    return Status::Ok;
  }

private:
  std::vector<Keyed> items_;
  std::size_t index_ = 0;
};

struct KeyedOutput {
  EventKey key;
  m::Output output;
};

class MemoryRecorder {
public:
  Status record(const EventKey& key, const m::Event& event) {
    inputs.push_back(Keyed{key, event});
    return Status::Ok;
  }
  Status emit(const EventKey& key, const m::Output& output) {
    outputs.push_back(KeyedOutput{key, output});
    return Status::Ok;
  }
  std::vector<Keyed> inputs;
  std::vector<KeyedOutput> outputs;
};

// Buys at market on the first quote; logs what it sees with the kernel time.
struct Buyer {
  std::vector<std::string>* log = nullptr;
  bool sent = false;
  static Status on_start(st::Context& ctx) { return ctx.subscribe_quotes(btc()); }
  Status on_quote(st::Context& ctx, const m::QuoteTick& q) {
    log->push_back("quote@" + std::to_string(ctx.now().value()) + " init " +
                   std::to_string(q.ts_init.value()));
    if (!sent) {
      sent = true;
      m::ClientOrderId id;
      return ctx.submit(ctx.market(btc(), m::OrderSide::Buy, qty("1.000")), id);
    }
    return Status::Ok;
  }
  Status on_order_event(st::Context& ctx, const m::OrderEvent& e) const {
    log->push_back(line(e).substr(0, line(e).find(' ')) + "@" + std::to_string(ctx.now().value()));
    return Status::Ok;
  }
};

std::vector<Keyed> market() {
  return {{EventKey{UnixNanos{1000}, 1, 1}, quote(1000, "100.0", "100.1")},
          {EventKey{UnixNanos{2000}, 1, 2}, quote(2000, "100.1", "100.2")},
          {EventKey{UnixNanos{5000}, 1, 3}, quote(5000, "100.2", "100.3")}};
}

struct VenueRun {
  std::vector<std::string> log;
  MemoryRecorder recorder;
  bt::RunSummary summary;
};

VenueRun run_venue(std::uint64_t seed, std::uint64_t jitter) {
  VenueRun out;
  VectorSource source{market()};
  bt::VenueLoopConfig vc;
  vc.sim = Venue::config(bt::FillModel::TopOfBook, bt::StpMode::None);
  vc.feed_ns = 100;
  vc.out_ns = 1000;
  vc.in_ns = 500;
  vc.jitter_ns = jitter;
  vc.seed = seed;
  vc.pending = 64;
  vc.commands = 64;
  vc.delta_pool = 256;
  bt::VenueLoop<VectorSource> loop{vc, source};
  REQUIRE(loop.exchange().on_data(m::Event{perpetual()}, UnixNanos{0}) == Status::Ok);
  st::KernelConfig kc;
  kc.instruments = 4;
  kc.strategies = 2;
  kc.trading.risk.orders_per_10s = 0;
  kc.trading.risk.orders_per_minute = 0;
  st::StaticStrategySet<Buyer> set{Buyer{&out.log}};
  jarvis::engine::Engine engine{kc, set};
  const std::array<m::Event, 1> preamble = {m::Event{perpetual()}};
  bt::DriverOptions options;
  options.preamble = preamble;
  bt::Driver driver{engine, loop, out.recorder, options};
  REQUIRE(driver.run(out.summary) == Status::Ok);
  return out;
}

std::vector<std::string> describe(const std::vector<Keyed>& inputs) {
  std::vector<std::string> out;
  out.reserve(inputs.size());
  for (const Keyed& k : inputs) {
    out.push_back(std::string{m::wire::kind_name(m::wire::kind_of(k.event))} + "@" +
                  std::to_string(k.key.ts.value()));
  }
  return out;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the venue loop delays data, commands and answers on their own channels") {
    const VenueRun r = run_venue(1, 0);
    // quote at 1000 seen at 1100; the market order reaches the venue at 2100, after the venue
    // saw the 2000 quote; the answers arrive at 2600.
    CHECK(r.log == std::vector<std::string>{"quote@1100 init 1100", "SUBMITTED@1100",
                                            "quote@2100 init 2100", "ACCEPTED@2600", "FILLED@2600",
                                            "quote@5100 init 5100"});
    const std::vector<std::string> inputs = describe(r.recorder.inputs);
    CHECK(std::count(inputs.begin(), inputs.end(), "CryptoPerpetual@1000") == 1); // preamble
    CHECK(std::find(inputs.begin(), inputs.end(), "OrderFilled@2600") != inputs.end());
    const auto filled =
        std::find_if(r.recorder.inputs.begin(), r.recorder.inputs.end(), [](const Keyed& k) {
          return std::holds_alternative<m::OrderFilled>(k.event);
        });
    REQUIRE(filled != r.recorder.inputs.end());
    const auto& f = std::get<m::OrderFilled>(filled->event);
    CHECK(f.last_px == px("100.2")); // the ask when the order arrived, not when it was sent
    CHECK(f.header.ts_event.value() == 2100);
    CHECK(f.header.ts_init.value() == 2600);
    CHECK(filled->key.source_id == bt::kVenueSource);
    CHECK(r.summary.venue_answers == 2);
    CHECK(r.summary.data_events == 3);
  }

  TEST_CASE("jittered delays are a function of the seed; the recording replays exactly") {
    const VenueRun a = run_venue(7, 400);
    const VenueRun b = run_venue(7, 400);
    const VenueRun c = run_venue(8, 400);
    CHECK(describe(a.recorder.inputs) == describe(b.recorder.inputs));
    CHECK(describe(a.recorder.inputs) != describe(c.recorder.inputs));

    // Stepping the recorded inputs alone (no venue, no driver) reproduces every output.
    st::KernelConfig kc;
    kc.instruments = 4;
    kc.strategies = 2;
    kc.trading.risk.orders_per_10s = 0;
    kc.trading.risk.orders_per_minute = 0;
    std::vector<std::string> log;
    st::StaticStrategySet<Buyer> set{Buyer{&log}};
    jarvis::engine::Engine engine{kc, set};
    std::size_t outputs = 0;
    for (const Keyed& k : a.recorder.inputs) {
      REQUIRE(engine.step(k.key, k.event) == Status::Ok);
      for (std::size_t i = 0; i < engine.outputs().size(); ++i, ++outputs) {
        REQUIRE(outputs < a.recorder.outputs.size());
        CHECK(engine.outputs()[i].index() == a.recorder.outputs[outputs].output.index());
      }
      engine.clear_outputs();
    }
    CHECK(outputs == a.recorder.outputs.size());
    CHECK(log == a.log);
  }
}

TEST_SUITE("property") {
  TEST_CASE("every venue answer is a transition the kernel's order state machine accepts") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      const bt::FillModel model =
          gen.coin() ? bt::FillModel::TopOfBook : bt::FillModel::QueuePosition;
      const bt::StpMode stp = static_cast<bt::StpMode>(gen.below(4));
      Venue v{model, stp};
      ex::Oms oms{512, 8192};
      std::vector<std::string> ids;
      std::int64_t mid = 1000; // in ticks of 0.1
      std::uint64_t applied = 0;
      const auto price_text = [](std::int64_t ticks) {
        return std::to_string(ticks / 10) + "." + std::to_string(ticks % 10);
      };
      const auto apply_all = [&]() {
        for (const m::OrderEvent& e : v.sim.events()) {
          std::uint32_t index = 0;
          const ex::EventOutcome outcome = ex::apply_order_event(oms, e, index);
          CAPTURE(line(e));
          REQUIRE(outcome == ex::EventOutcome::Applied);
          ++applied;
        }
      };
      for (int step = 0; step < 150; ++step) {
        const std::uint64_t action = gen.below(10);
        if (action < 4) {
          mid += static_cast<std::int64_t>(gen.below(5)) - 2;
          const std::int64_t spread = 1 + static_cast<std::int64_t>(gen.below(3));
          v.data(quote(0, price_text(mid), price_text(mid + spread),
                       std::to_string(1 + gen.below(3)) + ".000",
                       std::to_string(1 + gen.below(3)) + ".000"));
          apply_all();
        } else if (action < 6) {
          const m::AggressorSide side = gen.coin() ? m::AggressorSide::Buy : m::AggressorSide::Sell;
          v.data(trade(0, price_text(mid + static_cast<std::int64_t>(gen.below(5)) - 2),
                       "0." + std::to_string(100 + gen.below(900)), side));
          apply_all();
        } else if (action < 8) {
          const std::string id = "O" + std::to_string(ids.size());
          const m::OrderSide side = gen.coin() ? m::OrderSide::Buy : m::OrderSide::Sell;
          const bool market = gen.chance(1, 5);
          const std::array<m::TimeInForce, 4> tifs = {m::TimeInForce::Gtc, m::TimeInForce::Ioc,
                                                      m::TimeInForce::Fok, m::TimeInForce::Gtc};
          const m::TimeInForce tif = tifs[gen.below(tifs.size())];
          const bool post_only = !market && tif == m::TimeInForce::Gtc && gen.coin();
          const std::string price = price_text(mid + static_cast<std::int64_t>(gen.below(7)) - 3);
          const m::Output o = submit(id, side, std::to_string(1 + gen.below(3)) + ".000",
                                     market ? std::nullopt : std::optional<std::string_view>{price},
                                     tif, post_only);
          ex::OrderRecord r;
          r.client_order_id = cid(id);
          r.side = side;
          r.price = market ? std::nullopt : std::optional{px(price)};
          r.state = ex::OrderState{std::get<m::SubmitOrder>(o).quantity};
          std::uint32_t index = 0;
          REQUIRE(oms.create(r, index) == Status::Ok);
          REQUIRE(oms.apply(index, ex::OrderEventKind::Submitted) == Status::Ok);
          ids.push_back(id);
          v.command(o);
          apply_all();
        } else if (!ids.empty()) {
          const std::string& id = ids[gen.below(ids.size())];
          const std::uint32_t index = oms.find(cid(id));
          const m::OrderStatus status = oms.at(index).state.status();
          if (!ex::is_open(status) || ex::is_pending(status) ||
              status == m::OrderStatus::Submitted) {
            continue; // the kernel would not send this command now
          }
          if (action == 8) {
            REQUIRE(oms.apply(index, ex::OrderEventKind::PendingCancel) == Status::Ok);
            m::CancelOrder c;
            c.client_order_id = cid(id);
            c.instrument_id = btc();
            v.command(m::Output{c});
          } else {
            REQUIRE(oms.apply(index, ex::OrderEventKind::PendingUpdate) == Status::Ok);
            m::ModifyOrder mo;
            mo.client_order_id = cid(id);
            mo.instrument_id = btc();
            mo.quantity = m::Quantity{};
            REQUIRE(m::Quantity::from_raw(oms.at(index).state.quantity().raw() + 1'000'000'000, 3,
                                          mo.quantity) == Status::Ok);
            mo.price = px(price_text(mid + static_cast<std::int64_t>(gen.below(5)) - 2));
            v.command(m::Output{mo});
          }
          apply_all();
        }
      }
      CHECK(applied > 0);
    });
  }
}

namespace {

// A live source: the pump pushes what arrived; nothing yet is WouldBlock, not the end.
class PushSource {
public:
  void push(EventKey key, const m::Event& event) { items_.push_back(Keyed{key, event}); }
  Status next(EventKey& key, m::Event& event) {
    if (index_ >= items_.size()) {
      return Status::WouldBlock;
    }
    key = items_[index_].key;
    event = items_[index_].event;
    ++index_;
    return Status::Ok;
  }

private:
  std::vector<Keyed> items_;
  std::size_t index_ = 0;
};

// A virtual clock that jumps to the next arrival (or by `step` when idle) and delivers each
// arrival stamped with the clock, as the live pump does with what the IO threads hand over.
class FakePump {
public:
  FakePump(std::vector<Keyed> arrivals, PushSource& source, std::uint64_t start, std::uint64_t stop)
      : arrivals_{std::move(arrivals)}, source_{&source}, now_{start}, stop_{stop} {}
  [[nodiscard]] UnixNanos now() const { return UnixNanos{now_}; }
  Status pump(UnixNanos now) {
    while (next_ < arrivals_.size() && !(now < arrivals_[next_].key.ts)) {
      Keyed k = arrivals_[next_++];
      k.key.ts = now;
      source_->push(k.key, k.event);
    }
    return Status::Ok;
  }
  Status idle(UnixNanos /*now*/) {
    ++idles;
    std::uint64_t next = now_ + 250;
    if (next_ < arrivals_.size() && arrivals_[next_].key.ts.value() < next) {
      next = arrivals_[next_].key.ts.value();
    }
    now_ = next;
    return Status::Ok;
  }
  [[nodiscard]] bool stop_requested() const { return now_ >= stop_; }
  std::uint64_t idles = 0;

private:
  std::vector<Keyed> arrivals_;
  PushSource* source_;
  std::uint64_t now_;
  std::uint64_t stop_;
  std::size_t next_ = 0;
};

VenueRun run_realtime_venue(std::uint64_t seed, std::uint64_t jitter, std::uint64_t& idles) {
  VenueRun out;
  PushSource source;
  bt::VenueLoopConfig vc;
  vc.sim = Venue::config(bt::FillModel::TopOfBook, bt::StpMode::None);
  vc.out_ns = 1000;
  vc.in_ns = 500;
  vc.jitter_ns = jitter;
  vc.seed = seed;
  vc.pending = 64;
  vc.commands = 64;
  vc.delta_pool = 256;
  vc.live_feed = true;
  bt::VenueLoop<PushSource> loop{vc, source};
  REQUIRE(loop.exchange().on_data(m::Event{perpetual()}, UnixNanos{0}) == Status::Ok);
  st::KernelConfig kc;
  kc.instruments = 4;
  kc.strategies = 2;
  kc.trading.risk.orders_per_10s = 0;
  kc.trading.risk.orders_per_minute = 0;
  st::StaticStrategySet<Buyer> set{Buyer{&out.log}};
  jarvis::engine::Engine engine{kc, set};
  const std::array<m::Event, 1> preamble = {m::Event{perpetual()}};
  bt::DriverOptions options;
  options.preamble = preamble;
  bt::Driver driver{engine, loop, out.recorder, options};
  FakePump pump{market(), source, 1000, 8000};
  REQUIRE(driver.run_realtime(pump, out.summary) == Status::Ok);
  idles = pump.idles;
  return out;
}

VenueRun run_backtest_live_feed(std::uint64_t seed, std::uint64_t jitter) {
  VenueRun out;
  VectorSource source{market()};
  bt::VenueLoopConfig vc;
  vc.sim = Venue::config(bt::FillModel::TopOfBook, bt::StpMode::None);
  vc.out_ns = 1000;
  vc.in_ns = 500;
  vc.jitter_ns = jitter;
  vc.seed = seed;
  vc.pending = 64;
  vc.commands = 64;
  vc.delta_pool = 256;
  vc.live_feed = true;
  bt::VenueLoop<VectorSource> loop{vc, source};
  REQUIRE(loop.exchange().on_data(m::Event{perpetual()}, UnixNanos{0}) == Status::Ok);
  st::KernelConfig kc;
  kc.instruments = 4;
  kc.strategies = 2;
  kc.trading.risk.orders_per_10s = 0;
  kc.trading.risk.orders_per_minute = 0;
  st::StaticStrategySet<Buyer> set{Buyer{&out.log}};
  jarvis::engine::Engine engine{kc, set};
  const std::array<m::Event, 1> preamble = {m::Event{perpetual()}};
  bt::DriverOptions options;
  options.preamble = preamble;
  bt::Driver driver{engine, loop, out.recorder, options};
  REQUIRE(driver.run(out.summary) == Status::Ok);
  return out;
}

std::vector<std::byte> encoded(const Keyed& k) {
  std::vector<std::byte> buffer(m::wire::kRecordHeaderSize + m::wire::kMaxPayload + 4);
  std::size_t written = 0;
  REQUIRE(m::wire::encode_record(k.key, k.event, buffer, written) == Status::Ok);
  buffer.resize(written);
  return buffer;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("a real-time run records what a backtest of the same arrivals records") {
    for (const std::uint64_t jitter : {std::uint64_t{0}, std::uint64_t{700}}) {
      std::uint64_t idles = 0;
      const VenueRun live = run_realtime_venue(3, jitter, idles);
      const VenueRun back = run_backtest_live_feed(3, jitter);
      CHECK(idles > 0);
      CHECK(live.log == back.log);
      // Everything up to the end of the data matches record for record. The endings differ:
      // the backtest closes the last batch with EndOfData and Drained inside it; the real-time
      // run closed it when it went idle, and later stops (ShutdownRequested, Drained, BatchEnd).
      REQUIRE(live.recorder.inputs.size() >= 4);
      REQUIRE(back.recorder.inputs.size() >= 3);
      std::vector<Keyed> a{live.recorder.inputs.begin(), live.recorder.inputs.end() - 4};
      std::vector<Keyed> b{back.recorder.inputs.begin(), back.recorder.inputs.end() - 3};
      REQUIRE(a.size() == b.size());
      for (std::size_t i = 0; i < a.size(); ++i) {
        INFO("input " << i << " " << describe({a[i]})[0] << " vs " << describe({b[i]})[0]);
        CHECK(encoded(a[i]) == encoded(b[i]));
      }
      CHECK(live.recorder.outputs.size() == back.recorder.outputs.size());
      CHECK(live.summary.venue_answers == back.summary.venue_answers);
      CHECK(live.summary.venue_answers >= 2); // accepted and filled
      const auto& inputs = live.recorder.inputs;
      CHECK(std::get<m::NodeLifecycle>(inputs[inputs.size() - 3].event).reason ==
            m::LifecycleReason::ShutdownRequested);
      CHECK(std::get<m::NodeLifecycle>(inputs[inputs.size() - 2].event).reason ==
            m::LifecycleReason::Drained);
      CHECK(std::holds_alternative<m::BatchEnd>(inputs.back().event));
    }
  }
}
