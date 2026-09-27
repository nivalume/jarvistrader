#include "jarvis/node/corpus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "jarvis/core/int_math.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::node {

namespace m = jarvis::model;
using core::Status;

namespace {

constexpr std::uint64_t kStartNs = 1'767'225'600'000'000'000ULL; // 2026-01-01T00:00:00Z
constexpr std::string_view kAccount = "BINANCE-USDM-001";

template <typename T> T must(Status status, const T& value) {
  // Corpus inputs are constants chosen to be valid; a failure is a programming error.
  if (!core::ok(status)) {
    __builtin_trap();
  }
  return value;
}

m::InstrumentId iid(std::string_view text) {
  m::InstrumentId id;
  return must(m::InstrumentId::parse(text, id), id);
}
m::Currency ccy(std::string_view code) {
  m::Currency c;
  return must(m::Currency::builtin(code, c), c);
}
m::Price px(std::string_view text) {
  m::Price p;
  return must(m::Price::parse(text, p), p);
}
m::Quantity qty(std::string_view text) {
  m::Quantity q;
  return must(m::Quantity::parse(text, q), q);
}
m::Money money(std::string_view text) {
  m::Money v;
  return must(m::Money::parse(text, v), v);
}
m::Decimal dec(std::string_view text) {
  m::Decimal d;
  return must(m::Decimal::parse(text, d), d);
}
template <typename Id> Id make_id(std::string_view text) {
  Id id;
  return must(Id::from(text, id), id);
}

// Raw price (10^9 scale) around which an instrument's prices are drawn.
std::int64_t base_price_raw(std::string_view symbol) {
  if (symbol.starts_with("BTC")) {
    return 65'000'000'000'000;
  }
  if (symbol.starts_with("ETH")) {
    return 3'400'000'000'000;
  }
  return 150'000'000'000;
}

m::CryptoPerpetual perpetual(std::string_view id, std::string_view raw, std::string_view base,
                             std::string_view tick, std::string_view step) {
  m::CryptoPerpetual p;
  m::InstrumentCommon& c = p.common;
  c.id = iid(id);
  c.raw_symbol = make_id<m::Symbol>(raw);
  c.base_currency = ccy(base);
  c.quote_currency = ccy("USDT");
  c.settlement_currency = ccy("USDT");
  c.price_increment = px(tick);
  c.price_precision = c.price_increment.precision();
  c.size_increment = qty(step);
  c.size_precision = c.size_increment.precision();
  c.multiplier = qty("1");
  c.margin_init = dec("0.05");
  c.margin_maint = dec("0.025");
  return p;
}

} // namespace

CorpusGenerator::CorpusGenerator(std::uint64_t seed) : rng_{seed}, ts_{kStartNs} {
  instruments_[0] = perpetual("BTCUSDT-PERP.BINANCE", "BTCUSDT", "BTC", "0.1", "0.001");
  instruments_[1] = perpetual("ETHUSDT-PERP.BINANCE", "ETHUSDT", "ETH", "0.01", "0.001");
  instruments_[2] = perpetual("SOLUSDT-PERP.BINANCE", "SOLUSDT", "SOL", "0.001", "1");
}

std::uint64_t CorpusGenerator::draw(std::uint32_t hop, std::uint32_t index) const noexcept {
  return rng_.draw(seq_, hop, index);
}

const m::CryptoPerpetual& CorpusGenerator::instrument(std::uint32_t hop) const noexcept {
  return instruments_[draw(hop) % instruments_.size()];
}

m::Price CorpusGenerator::price_near(const m::CryptoPerpetual& inst, std::uint32_t hop) const {
  // Base price by instrument, moved by up to +-5000 ticks, then round-tripped through text so the
  // parser and formatter are part of the corpus.
  const std::int64_t tick = inst.common.price_increment.raw();
  const std::int64_t base = base_price_raw(inst.common.id.symbol.view());
  const auto offset = static_cast<std::int64_t>(draw(hop) % 10'001U) - 5'000;
  m::Price p;
  static_cast<void>(m::Price::from_raw(base + offset * tick, inst.common.price_precision, p));
  std::array<char, m::kMaxDecimalText> text{};
  std::size_t n = 0;
  static_cast<void>(p.format(text, n));
  m::Price parsed;
  return must(
      m::Price::parse(std::string_view{text.data(), n}, inst.common.price_precision, parsed),
      parsed);
}

m::Quantity CorpusGenerator::size_of(const m::CryptoPerpetual& inst, std::uint32_t hop) const {
  const std::uint64_t steps = 1 + draw(hop) % 5'000U;
  m::Quantity q;
  return must(m::Quantity::from_raw(steps * inst.common.size_increment.raw(),
                                    inst.common.size_precision, q),
              q);
}

m::OrderEventHeader CorpusGenerator::order_header(const m::CryptoPerpetual& inst) {
  m::OrderEventHeader h;
  h.trader_id = make_id<m::TraderId>("JARVIS-001");
  h.strategy_id = make_id<m::StrategyId>(draw(40) % 2 == 0 ? "MM-001" : "TREND-002");
  h.instrument_id = inst.common.id;
  static_cast<void>(
      m::ClientOrderIdGenerator::format("c0rp", 1, 1 + draw(41) % 50'000U, h.client_order_id));
  h.event_id = m::Uuid4::derive(rng_, seq_, 42);
  h.ts_event = ts_;
  h.ts_init = ts_;
  if (draw(43) % 4 == 0) {
    h.causation_id = m::Uuid4::derive(rng_, seq_, 44);
  }
  h.reconciliation = draw(45) % 16 == 0;
  return h;
}

// One function per event kind. Hops (the second RNG coordinate) are distinct per field so that
// fields of one event are independent draws.
struct CorpusMakers {
  using Perp = m::CryptoPerpetual;

  static m::OrderSide side(const CorpusGenerator& g, std::uint32_t hop) {
    return g.draw(hop) % 2 == 0 ? m::OrderSide::Buy : m::OrderSide::Sell;
  }
  static m::AccountId account() { return make_id<m::AccountId>(kAccount); }
  static m::VenueOrderId venue_order(const CorpusGenerator& g) {
    return make_id<m::VenueOrderId>(std::to_string(8'000'000'000ULL + g.draw(60) % 1'000'000U));
  }
  static m::ReasonText reason(std::string_view text) {
    m::ReasonText r;
    return must(m::ReasonText::from(text, r), r);
  }

  static Status trade(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::TradeTick t;
    t.instrument_id = inst.common.id;
    t.price = g.price_near(inst, 5);
    t.size = g.size_of(inst, 6);
    t.aggressor_side = g.draw(7) % 2 == 0 ? m::AggressorSide::Buy : m::AggressorSide::Sell;
    t.trade_id = make_id<m::TradeId>(std::to_string(1'000'000'000ULL + g.seq_));
    t.ts_event = g.ts_;
    t.ts_init = g.ts_;
    event = t;
    return Status::Ok;
  }

  static Status quote(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::QuoteTick q;
    q.instrument_id = inst.common.id;
    q.bid_price = g.price_near(inst, 8);
    m::Price spread;
    static_cast<void>(m::Price::from_raw(inst.common.price_increment.raw() *
                                             static_cast<std::int64_t>(1 + g.draw(9) % 5U),
                                         inst.common.price_precision, spread));
    static_cast<void>(m::Price::add(q.bid_price, spread, q.ask_price));
    q.bid_size = g.size_of(inst, 10);
    q.ask_size = g.size_of(inst, 11);
    q.ts_event = g.ts_;
    q.ts_init = g.ts_;
    event = q;
    return Status::Ok;
  }

  static m::BookAction book_action(std::uint64_t draw) {
    constexpr std::array<m::BookAction, 3> kActions = {m::BookAction::Add, m::BookAction::Update,
                                                       m::BookAction::Delete};
    return kActions[draw % kActions.size()];
  }

  static Status deltas(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    const std::size_t count = 1 + g.draw(12) % g.deltas_.size();
    const auto mbp = m::flag_bit(m::RecordFlag::F_MBP);
    const auto last = m::flag_bit(m::RecordFlag::F_LAST);
    for (std::size_t i = 0; i < count; ++i) {
      const auto index = static_cast<std::uint32_t>(i);
      m::OrderBookDelta& d = g.deltas_[i];
      d.instrument_id = inst.common.id;
      d.action = book_action(g.draw(13, index));
      d.order.side = g.draw(14, index) % 2 == 0 ? m::OrderSide::Buy : m::OrderSide::Sell;
      d.order.price = g.price_near(inst, 100 + index);
      d.order.size =
          d.action == m::BookAction::Delete ? m::Quantity{} : g.size_of(inst, 120 + index);
      d.order.order_id = 0;
      d.flags = static_cast<std::uint8_t>(i + 1 == count ? (last | mbp) : mbp);
      d.sequence = g.seq_ * 100 + i;
      d.ts_event = g.ts_;
      d.ts_init = g.ts_;
    }
    m::OrderBookDeltas batch;
    const Status s = m::OrderBookDeltas::create(
        std::span<const m::OrderBookDelta>{g.deltas_.data(), count}, batch);
    if (core::ok(s)) {
      event = batch;
    }
    return s;
  }

  static Status bar(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::BarType type;
    type.instrument_id = inst.common.id;
    type.spec =
        m::BarSpecification{1 + g.draw(17) % 15U, m::BarAggregation::Minute, m::PriceType::Last};
    type.aggregation_source = m::AggregationSource::External;
    const m::Price a = g.price_near(inst, 18);
    const m::Price b = g.price_near(inst, 19);
    const m::Price lo = a < b ? a : b;
    const m::Price hi = a < b ? b : a;
    m::Bar value;
    const Status s = m::Bar::create(type, lo, hi, lo, hi, g.size_of(inst, 20), g.ts_, g.ts_, value);
    if (core::ok(s)) {
      event = value;
    }
    return s;
  }

  static Status mark(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::MarkPriceUpdate{inst.common.id, g.price_near(inst, 21), g.ts_, g.ts_};
    return Status::Ok;
  }

  static Status index(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::IndexPriceUpdate{inst.common.id, g.price_near(inst, 22), g.ts_, g.ts_};
    return Status::Ok;
  }

  static Status funding(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::FundingRateUpdate f;
    f.instrument_id = inst.common.id;
    static_cast<void>(m::Decimal::from_raw(
        static_cast<std::int64_t>(g.draw(23) % 2'000U) * 100 - 100'000, 8, f.rate));
    f.interval = static_cast<std::uint16_t>(480);
    f.next_funding_ns = core::UnixNanos{g.ts_.value() + 8ULL * 3600 * core::kNanosPerSecond};
    f.ts_event = g.ts_;
    f.ts_init = g.ts_;
    event = f;
    return Status::Ok;
  }

  static Status status(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::InstrumentStatus st;
    st.instrument_id = inst.common.id;
    st.action = g.draw(24) % 2 == 0 ? m::MarketStatusAction::Trading : m::MarketStatusAction::Halt;
    st.ts_event = g.ts_;
    st.ts_init = g.ts_;
    st.is_trading = st.action == m::MarketStatusAction::Trading;
    event = st;
    return Status::Ok;
  }

  static Status close(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::InstrumentClose c;
    c.instrument_id = inst.common.id;
    c.close_price = g.price_near(inst, 61);
    c.close_type = g.draw(62) % 2 == 0 ? m::InstrumentCloseType::EndOfSession
                                       : m::InstrumentCloseType::ContractExpired;
    c.ts_event = g.ts_;
    c.ts_init = g.ts_;
    event = c;
    return Status::Ok;
  }

  static Status liquidation(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::LiquidationOrder l;
    l.instrument_id = inst.common.id;
    l.side = side(g, 25);
    l.price = g.price_near(inst, 26);
    l.quantity = g.size_of(inst, 27);
    l.average_price = l.price;
    l.filled_quantity = l.quantity;
    l.ts_event = g.ts_;
    l.ts_init = g.ts_;
    event = l;
    return Status::Ok;
  }

  static Status initialized(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::OrderInitialized o;
    o.header = g.order_header(inst);
    o.order_side = side(g, 28);
    o.order_type = m::OrderType::Limit;
    o.quantity = g.size_of(inst, 29);
    o.time_in_force = g.draw(30) % 3 == 0 ? m::TimeInForce::Ioc : m::TimeInForce::Gtc;
    o.post_only = g.draw(31) % 2 == 0;
    o.price = g.price_near(inst, 32);
    if (g.draw(33) % 8 == 0) {
      m::TagsText tags;
      o.tags = must(m::TagsText::from("corpus", tags), tags);
    }
    if (g.draw(50) % 6 == 0) {
      core::UnixNanos expire;
      static_cast<void>(g.ts_.plus(core::DurationNanos{3'600'000'000'000ULL}, expire));
      o.order_type = m::OrderType::StopLimit;
      o.trigger_price = g.price_near(inst, 51);
      o.trigger_type = m::TriggerType::LastPrice;
      o.reduce_only = true;
      o.time_in_force = m::TimeInForce::Gtd;
      o.expire_time = expire;
      o.display_qty = g.size_of(inst, 52);
      o.contingency_type = m::ContingencyType::Oto;
      o.exec_algorithm_id = make_id<m::ExecAlgorithmId>("PeggedQuote");
      o.parent_order_id = o.header.client_order_id;
    }
    event = o;
    return Status::Ok;
  }

  static Status denied(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderDenied{g.order_header(inst), reason("RISK_NOTIONAL_EXCEEDS_LIMIT")};
    return Status::Ok;
  }

  static Status emulated(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderEmulated{g.order_header(inst)};
    return Status::Ok;
  }

  static Status released(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderReleased{g.order_header(inst), g.price_near(inst, 63)};
    return Status::Ok;
  }

  static Status submitted(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderSubmitted{g.order_header(inst), account()};
    return Status::Ok;
  }

  static Status accepted(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    const m::VenueOrderId id = make_id<m::VenueOrderId>(std::to_string(9'000'000'000ULL + g.seq_));
    event = m::OrderAccepted{g.order_header(inst), id, account()};
    return Status::Ok;
  }

  static Status rejected(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderRejected{g.order_header(inst), account(), reason("GTX order rejected (-5022)"),
                             true};
    return Status::Ok;
  }

  static Status canceled(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::OrderCanceled c;
    c.header = g.order_header(inst);
    c.account_id = account();
    if (g.draw(53) % 2 == 0) {
      c.venue_order_id = venue_order(g);
    }
    event = c;
    return Status::Ok;
  }

  static Status expired(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderExpired{g.order_header(inst), venue_order(g), account()};
    return Status::Ok;
  }

  static Status triggered(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderTriggered{g.order_header(inst), venue_order(g), std::nullopt};
    return Status::Ok;
  }

  static Status pending_update(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderPendingUpdate{g.order_header(inst), account(), venue_order(g)};
    return Status::Ok;
  }

  static Status pending_cancel(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderPendingCancel{g.order_header(inst), account(), std::nullopt};
    return Status::Ok;
  }

  static Status modify_rejected(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderModifyRejected{g.order_header(inst),
                                   reason("Order would immediately trigger (-2021)"),
                                   venue_order(g), account()};
    return Status::Ok;
  }

  static Status cancel_rejected(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    event = m::OrderCancelRejected{g.order_header(inst), reason("Unknown order sent (-2011)"),
                                   std::nullopt, account()};
    return Status::Ok;
  }

  static Status updated(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::OrderUpdated u;
    u.header = g.order_header(inst);
    u.venue_order_id = venue_order(g);
    u.account_id = account();
    u.quantity = g.size_of(inst, 64);
    u.price = g.price_near(inst, 65);
    if (g.draw(66) % 3 == 0) {
      u.trigger_price = g.price_near(inst, 67);
    }
    event = u;
    return Status::Ok;
  }

  // commission = notional * fee rate in integers: maker 2 bp, taker 5 bp, truncated to the
  // currency grid.
  static std::optional<m::Money> commission(const Perp& inst, const m::OrderFilled& f) {
    m::Money notional;
    if (!core::ok(m::notional_value(inst.common, f.last_qty, f.last_px, notional))) {
      return std::nullopt;
    }
    const std::int64_t bps = f.liquidity_side == m::LiquiditySide::Taker ? 5 : 2;
    std::int64_t fee_raw = 0;
    if (!core::ok(core::mul_div_i64(notional.raw(), bps, 1, 10'000, fee_raw))) {
      return std::nullopt;
    }
    m::Money fee;
    static_cast<void>(m::Money::from_raw_truncated(fee_raw, notional.currency(), fee));
    return fee;
  }

  static Status filled(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::OrderFilled f;
    f.header = g.order_header(inst);
    f.venue_order_id = venue_order(g);
    f.account_id = account();
    f.trade_id = make_id<m::TradeId>(std::to_string(2'000'000'000ULL + g.seq_));
    f.order_side = side(g, 35);
    f.order_type = m::OrderType::Limit;
    f.last_qty = g.size_of(inst, 36);
    f.last_px = g.price_near(inst, 37);
    f.currency = ccy("USDT");
    f.liquidity_side = g.draw(38) % 3 == 0 ? m::LiquiditySide::Taker : m::LiquiditySide::Maker;
    f.commission = commission(inst, f);
    if (g.draw(39) % 10 == 0) {
      f.info_flags = static_cast<std::uint8_t>(m::FillInfo::Lite);
    }
    event = f;
    return Status::Ok;
  }

  static Status fill_voided(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::OrderFillVoided v;
    v.header = g.order_header(inst);
    v.venue_order_id = venue_order(g);
    v.account_id = account();
    v.correction_id = make_id<m::TradeId>(std::to_string(3'000'000'000ULL + g.seq_));
    v.trade_id = make_id<m::TradeId>(std::to_string(2'000'000'000ULL + g.draw(68) % g.seq_));
    v.voided_qty = g.size_of(inst, 69);
    m::Money fee;
    static_cast<void>(m::Money::from_raw(
        static_cast<std::int64_t>(g.draw(70) % 1'000U) * 10'000'000, ccy("USDT"), fee));
    v.commission_voided = fee;
    v.order_side = side(g, 71);
    v.order_type = m::OrderType::Limit;
    v.last_px = g.price_near(inst, 72);
    v.currency = ccy("USDT");
    v.liquidity_side = m::LiquiditySide::Maker;
    v.reason = reason("venue trade bust");
    v.is_reopened = g.draw(73) % 2 == 0;
    v.info_flags = static_cast<std::uint8_t>(m::FillInfo::Lite);
    event = v;
    return Status::Ok;
  }

  static Status account_state(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    const m::Currency usdt = ccy("USDT");
    m::Money total;
    m::Money locked;
    m::Money free;
    static_cast<void>(m::Money::from_raw(
        static_cast<std::int64_t>(g.draw(46) % 1'000'000U) * 1'000'000'000, usdt, total));
    static_cast<void>(m::Money::from_raw(total.raw() / 4 / 10 * 10, usdt, locked));
    static_cast<void>(m::Money::sub(total, locked, free));
    static_cast<void>(m::AccountBalance::create(total, locked, free, g.balances_[0]));
    g.margins_[0] = m::MarginBalance{locked, locked, usdt, inst.common.id};
    m::AccountState state;
    state.account_id = account();
    state.account_type = m::AccountType::Margin;
    state.base_currency = usdt;
    state.balances = std::span<const m::AccountBalance>{g.balances_.data(), 1};
    state.margins = std::span<const m::MarginBalance>{g.margins_.data(), 1};
    state.is_reported = true;
    state.event_id = m::Uuid4::derive(g.rng_, g.seq_, 47);
    state.ts_event = g.ts_;
    state.ts_init = g.ts_;
    event = state;
    return Status::Ok;
  }

  static Status timer(CorpusGenerator& g, const Perp& /*inst*/, m::Event& event) {
    event =
        m::TimerFired{core::TimerKey{static_cast<std::uint32_t>(g.draw(48) % 4U), 7}, g.ts_, g.ts_};
    return Status::Ok;
  }

  static Status batch_end(CorpusGenerator& g, const Perp& /*inst*/, m::Event& event) {
    event = m::BatchEnd{g.seq_, g.ts_};
    return Status::Ok;
  }

  static Status lifecycle(CorpusGenerator& g, const Perp& /*inst*/, m::Event& event) {
    event = m::NodeLifecycle{m::NodeState::Running, m::NodeState::Degraded,
                             m::LifecycleReason::HealthLost, g.ts_};
    return Status::Ok;
  }

  static Status strategy_error(CorpusGenerator& g, const Perp& /*inst*/, m::Event& event) {
    event = m::StrategyError{1, m::StrategyErrorKind::Overrun, g.draw(49), g.ts_};
    return Status::Ok;
  }

  static Status shutdown(CorpusGenerator& g, const Perp& /*inst*/, m::Event& event) {
    const m::ShutdownMode mode =
        g.draw(74) % 2 == 0 ? m::ShutdownMode::CancelAllThenExit : m::ShutdownMode::ExitKeepOrders;
    event = m::Shutdown{mode, g.ts_};
    return Status::Ok;
  }

  static Status rate_limit(CorpusGenerator& g, const Perp& /*inst*/, m::Event& event) {
    const bool orders = g.draw(76) % 2 == 0;
    event = m::RateLimitFeedback{
        orders ? m::RateLimitKind::Orders : m::RateLimitKind::RequestWeight,
        orders ? 10'000'000'000ULL : 60'000'000'000ULL,
        static_cast<std::uint32_t>(g.draw(77) % 300U), orders ? 300U : 2400U, g.ts_};
    return Status::Ok;
  }

  // Instrument definitions: the drawn perpetual re-stamped, a spot pair and a dated future.
  static Status perpetual_def(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::CryptoPerpetual p = inst;
    p.common.ts_event = g.ts_;
    p.common.ts_init = g.ts_;
    if (g.draw(75) % 2 == 0) {
      p.common.min_notional = money("5 USDT");
    }
    event = p;
    return Status::Ok;
  }

  static Status pair_def(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::CurrencyPair pair;
    pair.common = inst.common;
    const std::string_view spot = inst.common.raw_symbol.view();
    pair.common.id = iid(std::string{spot} + ".BINANCE");
    pair.common.ts_event = g.ts_;
    pair.common.ts_init = g.ts_;
    event = pair;
    return Status::Ok;
  }

  static Status future_def(CorpusGenerator& g, const Perp& inst, m::Event& event) {
    m::CryptoFuture f;
    f.common = inst.common;
    f.common.id = iid(std::string{inst.common.raw_symbol.view()} + "_261225.BINANCE");
    f.underlying = inst.common.base_currency.value_or(ccy("BTC"));
    f.activation_ns = core::UnixNanos{kStartNs};
    f.expiration_ns = core::UnixNanos{1'798'156'800'000'000'000ULL}; // 2026-12-25T00:00:00Z
    f.common.ts_event = g.ts_;
    f.common.ts_init = g.ts_;
    event = f;
    return Status::Ok;
  }

  using Maker = Status (*)(CorpusGenerator&, const Perp&, m::Event&);
  struct Weighted {
    std::uint32_t weight; // per mille
    Maker make;
  };

  // A market-data-heavy mix in which every kind has at least 0.9%, so a few hundred events cover
  // all of them. Same order as model::Event.
  static constexpr std::array<Weighted, 37> kMix = {{
      {198, &trade},         {193, &quote},         {70, &deltas},          {35, &bar},
      {43, &mark},           {26, &index},          {17, &funding},         {9, &status},
      {10, &close},          {9, &liquidation},     {34, &initialized},     {10, &denied},
      {10, &emulated},       {10, &released},       {10, &submitted},       {17, &accepted},
      {17, &rejected},       {17, &canceled},       {10, &expired},         {10, &triggered},
      {10, &pending_update}, {10, &pending_cancel}, {10, &modify_rejected}, {10, &cancel_rejected},
      {10, &updated},        {52, &filled},         {10, &fill_voided},     {26, &account_state},
      {26, &timer},          {17, &batch_end},      {9, &lifecycle},        {9, &strategy_error},
      {10, &shutdown},       {9, &pair_def},        {9, &perpetual_def},    {9, &future_def},
      {9, &rate_limit},
  }};
};

namespace {

constexpr std::uint32_t total_weight() {
  std::uint32_t sum = 0;
  for (const CorpusMakers::Weighted& w : CorpusMakers::kMix) {
    sum += w.weight;
  }
  return sum;
}

} // namespace

static_assert(CorpusMakers::kMix.size() == std::variant_size_v<m::Event>);
static_assert(total_weight() == 1000);

Status CorpusGenerator::next(core::EventKey& key, m::Event& event) {
  ++seq_;
  core::UnixNanos next_ts;
  if (!core::ok(ts_.plus(core::DurationNanos{1 + draw(1) % 2'000'000U}, next_ts))) {
    return Status::Overflow;
  }
  ts_ = next_ts;
  key = core::EventKey{ts_, static_cast<std::uint16_t>(draw(2) % 4U), seq_};
  const m::CryptoPerpetual& inst = instrument(3);
  std::uint64_t roll = draw(4) % 1000U;
  for (const CorpusMakers::Weighted& w : CorpusMakers::kMix) {
    if (roll < w.weight) {
      return w.make(*this, inst, event);
    }
    roll -= w.weight;
  }
  return Status::InvalidState; // unreachable: the weights sum to 1000
}

} // namespace jarvis::node
