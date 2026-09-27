#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <doctest/doctest.h>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/risk/gates.hpp"
#include "jarvis/risk/monitors.hpp"
#include "jarvis/risk/rate_limit.hpp"
#include "jarvis/risk/trading_state.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace m = jarvis::model;
namespace r = jarvis::risk;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using m::TradingState;
using r::CommandKind;
using r::TradingTrigger;

constexpr std::uint64_t kSecond = 1'000'000'000ULL;
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
m::Money money(std::string_view text) {
  m::Money out;
  REQUIRE(m::Money::parse(text, out) == Status::Ok);
  return out;
}

m::InstrumentCommon perpetual() {
  m::InstrumentCommon c;
  m::Currency btc;
  m::Currency usdt;
  REQUIRE(m::Currency::builtin("BTC", btc) == Status::Ok);
  REQUIRE(m::Currency::builtin("USDT", usdt) == Status::Ok);
  c.base_currency = btc;
  c.quote_currency = usdt;
  c.settlement_currency = usdt;
  c.price_precision = 1;
  c.size_precision = 3;
  c.price_increment = px("0.5");
  c.size_increment = qty("0.002");
  c.multiplier = qty("1");
  c.min_quantity = qty("0.002");
  c.max_quantity = qty("100.000");
  c.min_price = px("1.0");
  c.max_price = px("1000000.0");
  c.min_notional = money("5 USDT");
  return c;
}

struct Fixture {
  m::InstrumentCommon instrument = perpetual();
  r::RiskConfig config;
  Fixture() {
    config.orders_per_10s = 0;
    config.orders_per_minute = 0;
  }

  r::OrderCheck order(std::string_view q, std::optional<std::string_view> price,
                      m::OrderSide side = m::OrderSide::Buy,
                      CommandKind kind = CommandKind::Open) const {
    r::OrderCheck c;
    c.instrument = &instrument;
    c.side = side;
    c.type = price ? m::OrderType::Limit : m::OrderType::Market;
    c.quantity = qty(q);
    if (price) {
      c.price = px(*price);
    }
    c.kind = kind;
    c.reference = px("100.0");
    c.now = UnixNanos{kSecond};
    return c;
  }
};

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the TradingState command matrix") {
    for (const CommandKind k : {CommandKind::Open, CommandKind::Reduce, CommandKind::Modify,
                                CommandKind::ModifyUp, CommandKind::Cancel}) {
      CHECK(r::allowed(TradingState::Active, k));
    }
    CHECK_FALSE(r::allowed(TradingState::Reducing, CommandKind::Open));
    CHECK(r::allowed(TradingState::Reducing, CommandKind::Reduce));
    CHECK(r::allowed(TradingState::Reducing, CommandKind::Modify));
    CHECK_FALSE(r::allowed(TradingState::Reducing, CommandKind::ModifyUp));
    CHECK(r::allowed(TradingState::Reducing, CommandKind::Cancel));
    CHECK_FALSE(r::allowed(TradingState::Halted, CommandKind::Reduce));
    CHECK_FALSE(r::allowed(TradingState::Halted, CommandKind::Modify));
    CHECK(r::allowed(TradingState::Halted, CommandKind::Cancel));
  }

  TEST_CASE("monitors only tighten; holds clear themselves; only an admin loosens") {
    r::TradingStateMachine s;
    CHECK(s.state() == TradingState::Active);
    CHECK(s.apply(TradingTrigger::SyncStarted));
    CHECK(s.state() == TradingState::Halted);
    CHECK(s.apply(TradingTrigger::Synced));
    CHECK(s.state() == TradingState::Active);
    CHECK(s.apply(TradingTrigger::SoftLimit));
    CHECK(s.state() == TradingState::Reducing);
    s.apply(TradingTrigger::SyncStarted);
    s.apply(TradingTrigger::Synced);
    CHECK(s.state() == TradingState::Reducing); // reconciling does not undo a monitor
    CHECK_FALSE(s.apply(TradingTrigger::SoftLimit));
    CHECK(s.apply(TradingTrigger::HardLimit));
    CHECK(s.state() == TradingState::Halted);
    CHECK_FALSE(s.apply(TradingTrigger::SoftLimit)); // never loosens
    CHECK(s.apply(TradingTrigger::AdminReduce));
    CHECK(s.state() == TradingState::Reducing);
    CHECK(s.apply(TradingTrigger::AdminResume));
    CHECK(s.state() == TradingState::Active);
    CHECK(s.apply(TradingTrigger::Degraded));
    CHECK(s.state() == TradingState::Reducing);
    CHECK(s.apply(TradingTrigger::Recovered));
    CHECK(s.state() == TradingState::Active);

    r::TradingStateMachine halted{TradingState::Halted};
    halted.apply(TradingTrigger::SyncStarted);
    halted.apply(TradingTrigger::Synced);
    CHECK(halted.state() == TradingState::Halted); // the configured initial state stays
  }

  TEST_CASE("rate windows count orders between multiples of their interval") {
    const std::array<r::RateWindow, 2> windows = {{{10 * kSecond, 3}, {60 * kSecond, 5}}};
    r::RateLimiter limiter{windows};
    CHECK(limiter.remaining(UnixNanos{kSecond}) == 3);
    CHECK(limiter.try_acquire(UnixNanos{kSecond}));
    CHECK(limiter.try_acquire(UnixNanos{2 * kSecond}));
    CHECK(limiter.try_acquire(UnixNanos{9 * kSecond}));
    CHECK_FALSE(limiter.try_acquire(UnixNanos{9 * kSecond}));
    CHECK(limiter.try_acquire(UnixNanos{10 * kSecond})); // a new 10 s window
    CHECK(limiter.try_acquire(UnixNanos{11 * kSecond}));
    CHECK_FALSE(limiter.try_acquire(UnixNanos{12 * kSecond})); // the minute is spent
    CHECK(limiter.remaining(UnixNanos{12 * kSecond}) == 0);
    CHECK(limiter.try_acquire(UnixNanos{60 * kSecond}));
    CHECK_FALSE(limiter.try_acquire(UnixNanos{60 * kSecond}, 3));
    CHECK(limiter.remaining(UnixNanos{60 * kSecond}) == 2); // a refused cost takes nothing
    const r::RateLimiter none;
    CHECK(none.windows() == 0);
  }

  TEST_CASE("the loss monitor: daily loss, hard limit, drawdown and margin ratio") {
    r::MonitorLimits limits;
    limits.daily_loss_raw = 10 * kRaw;
    limits.daily_halt_raw = 20 * kRaw;
    limits.drawdown_raw = 15 * kRaw;
    limits.margin_ratio_bps = 8000;
    r::LossMonitor monitor{limits};
    const std::uint64_t day = r::LossMonitor::kDayNs;
    CHECK_FALSE(monitor.observe(UnixNanos{day + 1}, 1000 * kRaw, 0));
    CHECK_FALSE(monitor.observe(UnixNanos{day + 2}, 1005 * kRaw, 0)); // new peak
    CHECK(monitor.observe(UnixNanos{day + 3}, 990 * kRaw, 0) == TradingTrigger::SoftLimit);
    CHECK(monitor.daily_loss_raw() == 10 * kRaw);
    CHECK(monitor.observe(UnixNanos{day + 4}, 979 * kRaw, 0) == TradingTrigger::HardLimit);
    // The next day starts from the current equity; the drawdown from 1005 stays.
    CHECK(monitor.observe(UnixNanos{2 * day}, 989 * kRaw, 0) == TradingTrigger::SoftLimit);
    CHECK(monitor.daily_loss_raw() == 0);
    CHECK(monitor.drawdown_raw() == 16 * kRaw);

    r::LossMonitor margin{r::MonitorLimits{std::nullopt, std::nullopt, std::nullopt, 8000}};
    CHECK_FALSE(margin.observe(UnixNanos{1}, 100 * kRaw, 79 * kRaw));
    CHECK(margin.observe(UnixNanos{2}, 100 * kRaw, 80 * kRaw) == TradingTrigger::SoftLimit);
    CHECK(margin.observe(UnixNanos{3}, -1, 1) == TradingTrigger::SoftLimit);
  }

  TEST_CASE("Gate B: price filter, lot size and notional limits") {
    Fixture f;
    r::RiskEngine engine{f.config, 4, 2};
    CHECK(engine.check_order(f.order("0.100", "100.0")).empty());
    CHECK(engine.check_order(f.order("0.100", "100.3")) == r::reason::kPriceInvalidTick);
    CHECK(engine.check_order(f.order("0.100", "0.5")) == r::reason::kPriceOutOfRange);
    CHECK(engine.check_order(f.order("0.101", "100.0")) == r::reason::kQuantityInvalidStep);
    CHECK(engine.check_order(f.order("200.000", "100.0")) == r::reason::kQuantityOutOfRange);
    CHECK(engine.check_order(f.order("0.040", "100.0")) == r::reason::kNotionalBelowMin);
    CHECK(engine.check_order(f.order("0.040", std::nullopt)) == r::reason::kNotionalBelowMin);

    f.config.max_order_notional = money("50 USDT");
    r::RiskEngine capped{f.config, 4, 2};
    CHECK(capped.check_order(f.order("0.600", "100.0")) == r::reason::kNotionalExceedsMax);
    f.config.max_order_notional = money("50 BTC"); // another currency: does not apply
    r::RiskEngine other{f.config, 4, 2};
    CHECK(other.check_order(f.order("0.600", "100.0")).empty());
  }

  TEST_CASE("Gate B: price band, open orders, reduce-only and margin") {
    Fixture f;
    f.config.price_band_bps = 100;
    f.config.max_open_orders = 2;
    r::RiskEngine engine{f.config, 4, 2};
    CHECK(engine.check_order(f.order("0.100", "101.0")).empty());
    CHECK(engine.check_order(f.order("0.100", "101.5")) == r::reason::kPriceOutsideBand);
    r::OrderCheck busy = f.order("0.100", "100.0");
    busy.open.orders = 2;
    CHECK(engine.check_order(busy) == r::reason::kOpenOrdersExceeded);

    r::OrderCheck reduce = f.order("0.040", "100.0", m::OrderSide::Sell);
    reduce.reduce_only = true;
    CHECK(engine.check_order(reduce) == r::reason::kReduceOnlyInvalid); // nothing to reduce
    reduce.kind = CommandKind::Reduce;
    reduce.position_raw = static_cast<std::int64_t>(qty("0.040").raw());
    CHECK(engine.check_order(reduce).empty()); // and exempt from the minimum notional

    r::OrderCheck costly = f.order("0.100", "100.0");
    costly.margin_known = true;
    costly.available_raw = 4 * kRaw;
    costly.required_raw = 5 * kRaw;
    CHECK(engine.check_order(costly) == r::reason::kMarginInsufficient);
    costly.kind = CommandKind::Reduce;
    CHECK(engine.check_order(costly).empty());
  }

  TEST_CASE("Gate A: TradingState, whitelist, instrument status, exposure") {
    Fixture f;
    f.config.max_position_notional = money("100 USDT");
    r::RiskEngine engine{f.config, 4, 2};
    engine.apply(TradingTrigger::SoftLimit);
    CHECK(engine.check_order(f.order("0.100", "100.0")) == r::reason::kTradingReducingOnly);
    r::OrderCheck reduce = f.order("0.100", "100.0", m::OrderSide::Sell, CommandKind::Reduce);
    CHECK(engine.check_order(reduce).empty());
    engine.apply(TradingTrigger::HardLimit);
    CHECK(engine.check_order(reduce) == r::reason::kTradingHalted);
    engine.apply(TradingTrigger::AdminResume);

    engine.restrict(0);
    engine.allow(0, 1);
    r::OrderCheck elsewhere = f.order("0.100", "100.0");
    CHECK(engine.check_order(elsewhere) == r::reason::kInstrumentNotAllowed);
    elsewhere.slot = 1;
    CHECK(engine.check_order(elsewhere).empty());
    elsewhere.strategy = 1; // an unrestricted strategy
    elsewhere.slot = 0;
    CHECK(engine.check_order(elsewhere).empty());

    m::InstrumentStatus halt;
    halt.action = m::MarketStatusAction::Halt;
    engine.on_status(2, halt);
    r::OrderCheck halted = f.order("0.100", "100.0");
    halted.strategy = 1;
    halted.slot = 2;
    CHECK(engine.check_order(halted) == r::reason::kInstrumentNotTrading);
    halted.kind = CommandKind::Reduce;
    halted.side = m::OrderSide::Sell;
    CHECK(engine.check_order(halted).empty());
    m::InstrumentStatus open;
    open.action = m::MarketStatusAction::Trading;
    engine.on_status(2, open);
    halted.kind = CommandKind::Open;
    CHECK(engine.check_order(halted).empty());

    // 0.600 held + 0.300 on open buys + 0.200 new = 1.100 x 100 > 100 USDT
    r::OrderCheck exposed = f.order("0.200", "100.0");
    exposed.strategy = 1;
    exposed.position_raw = static_cast<std::int64_t>(qty("0.600").raw());
    exposed.open.buy_raw = qty("0.300").raw();
    CHECK(engine.check_order(exposed) == r::reason::kExposureExceedsLimit);
    exposed.quantity = qty("0.100");
    CHECK(engine.check_order(exposed).empty());
  }

  TEST_CASE("the rate limit spends a token only when every other rule passed") {
    Fixture f;
    f.config.orders_per_10s = 2;
    r::RiskEngine engine{f.config, 4, 2};
    CHECK(engine.check_order(f.order("0.101", "100.0")) == r::reason::kQuantityInvalidStep);
    CHECK(engine.check_order(f.order("0.100", "100.0")).empty());
    CHECK(engine.check_order(f.order("0.100", "100.0")).empty());
    CHECK(engine.check_order(f.order("0.100", "100.0")) == r::reason::kRateLimitExceeded);
    CHECK(engine.stats().denied_b == 2);
  }

  TEST_CASE("modifies: quantity increases need Active; prices must stay on the grid") {
    Fixture f;
    r::RiskEngine engine{f.config, 4, 2};
    engine.apply(TradingTrigger::SoftLimit);
    r::OrderCheck down = f.order("0.100", "99.5", m::OrderSide::Buy, CommandKind::Modify);
    CHECK(engine.check_modify(down).empty());
    r::OrderCheck up = f.order("0.200", "99.5", m::OrderSide::Buy, CommandKind::ModifyUp);
    CHECK(engine.check_modify(up) == r::reason::kTradingReducingOnly);
    r::OrderCheck tick = f.order("0.100", "99.3", m::OrderSide::Buy, CommandKind::Modify);
    CHECK(engine.check_modify(tick) == r::reason::kPriceInvalidTick);
    CHECK(engine.stats().modify_rejected == 2);
  }
}

namespace {
int rank(TradingState s) { return r::detail::rank(s); }
} // namespace

TEST_SUITE("property") {
  TEST_CASE("automatic triggers never loosen the base state") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      r::TradingStateMachine s;
      constexpr std::array<TradingTrigger, 6> automatic = {
          TradingTrigger::SyncStarted, TradingTrigger::Synced,    TradingTrigger::Degraded,
          TradingTrigger::Recovered,   TradingTrigger::SoftLimit, TradingTrigger::HardLimit};
      int before = 0;
      for (int i = 0; i < 64; ++i) {
        s.apply(automatic[gen.below(automatic.size())]);
        const int now = rank(s.base());
        CHECK(now >= before);
        before = now;
        // The effective state is at least as strict as the base.
        CHECK(rank(s.state()) >= now);
        CHECK((!s.syncing() || s.state() == TradingState::Halted));
      }
    });
  }
}
