// Forward trace validation (docs/architecture.md 18.2): replays behaviours that TLC generated
// from a spec (tools/tla/behaviours.py) through the C++ implementation and compares, after every
// step, the implementation's state projected on the spec's variables. The mapping from spec
// actions to kernel inputs is the one in specs/map/<spec>_actions.hpp.
//
//   trace_driver <behaviours.txt>...
//
// Exit status 0 when every step matched and every action of the spec appeared at least once.

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "behaviour_file.hpp"
#include "jarvis/adapter/binance/depth_sync.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/backtest/matching/sim_exchange.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/engine/sync_gate.hpp"
#include "jarvis/execution/execution_engine.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/reports.hpp"
#include "jarvis/risk/gates.hpp"
#include "jarvis/risk/trading_state.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "specs/map/depth_sync_actions.hpp"
#include "specs/map/matching_actions.hpp"
#include "specs/map/order_lifecycle_actions.hpp"
#include "specs/map/reconciliation_actions.hpp"
#include "specs/map/trading_state_actions.hpp"

namespace {

namespace bt = jarvis::backtest;
namespace ex = jarvis::execution;
namespace m = jarvis::model;
namespace r = jarvis::risk;
namespace st = jarvis::strategy;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::trace::BehaviourFile;
using jarvis::trace::Step;

constexpr std::uint64_t kScale = 1'000'000'000ULL;

void require(Status s, std::string_view what) {
  if (!jarvis::core::ok(s)) {
    throw std::runtime_error(std::string{what} + " failed");
  }
}

// Collects the first differences between the spec's state and the implementation's.
class Diff {
public:
  template <typename A, typename B>
  void expect(std::string_view var, const A& spec, const B& impl) {
    if (!(spec == impl)) {
      std::ostringstream out;
      out << "  " << var << ": spec " << spec << ", implementation " << impl << "\n";
      text_ += out.str();
    }
  }
  void note(const std::string& line) { text_ += "  " + line + "\n"; }
  [[nodiscard]] const std::string& text() const { return text_; }

private:
  std::string text_;
};

m::InstrumentId instrument_id() {
  m::InstrumentId id;
  require(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id), "instrument id");
  return id;
}

m::Price price(std::int64_t raw, std::uint8_t precision) {
  m::Price p;
  require(m::Price::from_raw(raw, precision, p), "price");
  return p;
}

m::Quantity quantity(std::uint64_t raw, std::uint8_t precision) {
  m::Quantity q;
  require(m::Quantity::from_raw(raw, precision, q), "quantity");
  return q;
}

m::CryptoPerpetual perpetual(std::uint8_t size_precision) {
  m::CryptoPerpetual p;
  m::InstrumentCommon& c = p.common;
  c.id = instrument_id();
  require(m::Symbol::from("BTCUSDT", c.raw_symbol), "symbol");
  require(m::Currency::builtin("BTC", c.base_currency.emplace()), "currency");
  require(m::Currency::builtin("USDT", c.quote_currency), "currency");
  c.settlement_currency = c.quote_currency;
  c.price_precision = 1;
  c.size_precision = size_precision;
  c.price_increment = price(100'000'000, 1);
  c.size_increment = quantity(jarvis::core::kPow10[9 - size_precision], size_precision);
  c.multiplier = quantity(kScale, 0);
  require(m::Decimal::parse("0.05", c.margin_init), "margin");
  require(m::Decimal::parse("0.025", c.margin_maint), "margin");
  return p;
}

// ---- OrderLifecycle ----------------------------------------------------------------------------

// Order event kinds by spec name; the variant alternative is the kind plus one (order_fsm.hpp).
template <std::size_t I = 1>
m::OrderEvent event_at(std::size_t index, const m::OrderEventHeader& header) {
  if constexpr (I < std::variant_size_v<m::OrderEvent>) {
    if (index == I) {
      m::OrderEvent e{std::in_place_index<I>};
      std::visit([&header](auto& x) { x.header = header; }, e);
      return e;
    }
    return event_at<I + 1>(index, header);
  } else {
    throw std::logic_error("no order event alternative " + std::to_string(index));
  }
}

ex::OrderEventKind kind_named(std::string_view name) {
  for (std::size_t i = 0; i < ex::kOrderEventKindCount; ++i) {
    const auto k = static_cast<ex::OrderEventKind>(i);
    if (ex::to_string(k) == name) {
      return k;
    }
  }
  throw std::runtime_error("unknown order event kind " + std::string{name});
}

class OrderLifecycleReplayer {
public:
  static constexpr const auto& kActions = jarvis::specmap::order_lifecycle::kActions;

  explicit OrderLifecycleReplayer(const BehaviourFile& /*file*/) {
    require(m::ClientOrderId::from("O-1", header_.client_order_id), "client order id");
    header_.instrument_id = instrument_id();
  }

  std::string start(const Step& init) {
    ex::OrderRecord record;
    record.client_order_id = header_.client_order_id;
    record.instrument_id = header_.instrument_id;
    record.price = price(100 * static_cast<std::int64_t>(kScale), 0);
    record.state = ex::OrderState{units(init.integer("quantity"))};
    require(oms_.create(record, index_), "Oms::create");
    return compare(init);
  }

  std::string step(const Step& s) {
    Diff diff;
    refuses_disabled(s, diff);
    if (!diff.text().empty()) {
      return diff.text();
    }
    std::uint32_t index = ex::kNoIndex;
    const ex::EventOutcome outcome = ex::apply_order_event(oms_, event_for(s), index);
    if (outcome != ex::EventOutcome::Applied) {
      diff.note("the OMS did not apply the event (outcome " +
                std::to_string(static_cast<int>(outcome)) + ")");
      return diff.text();
    }
    return compare(s);
  }

private:
  static m::Quantity units(long long n) {
    return quantity(static_cast<std::uint64_t>(n) * kScale, 0);
  }

  [[nodiscard]] const ex::OrderState& state() const { return oms_.at(index_).state; }

  // Every kind without quantities that the spec did not enable in the state the step left must
  // be refused there, and leave the order as it was.
  void refuses_disabled(const Step& s, Diff& diff) {
    const std::set<std::string> enabled = s.set("plain");
    for (std::size_t i = 0; i < ex::kOrderEventKindCount; ++i) {
      const auto kind = static_cast<ex::OrderEventKind>(i);
      if (kind == ex::OrderEventKind::Filled || kind == ex::OrderEventKind::FillVoided ||
          kind == ex::OrderEventKind::Updated ||
          enabled.contains(std::string{ex::to_string(kind)})) {
        continue;
      }
      const std::string before = projection();
      std::uint32_t index = ex::kNoIndex;
      const ex::EventOutcome outcome = ex::apply_order_event(oms_, event_at(i + 1, header_), index);
      if (outcome != ex::EventOutcome::Refused || projection() != before) {
        diff.note("the spec does not enable Plain(" + std::string{ex::to_string(kind)} +
                  ") before this step, but the OMS applied it (" + before + ")");
        return;
      }
    }
  }

  m::OrderEvent event_for(const Step& s) const {
    if (s.action == "Plain") {
      return event_at(static_cast<std::size_t>(kind_named(s.args.at(0))) + 1, header_);
    }
    if (s.action == "Updated") {
      m::OrderUpdated e;
      e.header = header_;
      e.quantity = units(s.arg(0));
      return m::OrderEvent{e};
    }
    if (s.action == "Fill") {
      m::OrderFilled e;
      e.header = header_;
      require(m::TradeId::from(s.args.at(0), e.trade_id), "trade id");
      e.last_qty = units(s.arg(1));
      e.last_px = price(100 * static_cast<std::int64_t>(kScale), 0);
      e.liquidity_side = m::LiquiditySide::Maker;
      return m::OrderEvent{e};
    }
    if (s.action == "Void") {
      m::OrderFillVoided e;
      e.header = header_;
      require(m::TradeId::from(s.args.at(0), e.trade_id), "trade id");
      e.voided_qty = units(s.arg(1));
      e.last_px = price(100 * static_cast<std::int64_t>(kScale), 0);
      return m::OrderEvent{e};
    }
    throw std::runtime_error("unknown OrderLifecycle action " + s.action);
  }

  [[nodiscard]] std::string projection() const {
    const ex::OrderState& o = state();
    const std::optional<m::OrderStatus> prev = o.previous();
    return "status=" + std::string{m::to_string(o.status())} +
           " prev=" + (prev ? std::string{m::to_string(*prev)} : std::string{"NONE"}) +
           " quantity=" + std::to_string(o.quantity().raw() / kScale) +
           " filled=" + std::to_string(o.filled().raw() / kScale);
  }

  std::string compare(const Step& s) const {
    Diff diff;
    const ex::OrderState& o = state();
    diff.expect("status", s.var("status"), std::string{m::to_string(o.status())});
    const std::optional<m::OrderStatus> prev = o.previous();
    diff.expect("prev", s.var("prev"),
                prev ? std::string{m::to_string(*prev)} : std::string{"NONE"});
    diff.expect("quantity", s.integer("quantity"),
                static_cast<long long>(o.quantity().raw() / kScale));
    diff.expect("filled", s.integer("filled"), static_cast<long long>(o.filled().raw() / kScale));
    return diff.text();
  }

  ex::Oms oms_{4, 64};
  std::uint32_t index_ = ex::kNoIndex;
  m::OrderEventHeader header_;
};

// ---- TradingState ------------------------------------------------------------------------------

constexpr std::uint64_t kWindowNs = 10'000'000'000ULL; // RiskConfig::orders_per_10s

r::TradingTrigger trigger_named(std::string_view name) {
  for (std::size_t i = 0; i < jarvis::specmap::trading_state::kTriggers.size(); ++i) {
    if (jarvis::specmap::trading_state::kTriggers[i] == name) {
      return static_cast<r::TradingTrigger>(i);
    }
  }
  throw std::runtime_error("unknown trigger " + std::string{name});
}

r::CommandKind command_named(std::string_view name) {
  for (std::size_t i = 0; i < jarvis::specmap::trading_state::kCommands.size(); ++i) {
    if (jarvis::specmap::trading_state::kCommands[i] == name) {
      return static_cast<r::CommandKind>(i);
    }
  }
  throw std::runtime_error("unknown command " + std::string{name});
}

m::TradingState state_named(std::string_view name) {
  for (const m::TradingState s :
       {m::TradingState::Active, m::TradingState::Reducing, m::TradingState::Halted}) {
    if (r::spec_name(s) == name) {
      return s;
    }
  }
  throw std::runtime_error("unknown TradingState " + std::string{name});
}

class TradingStateReplayer {
public:
  static constexpr const auto& kActions = jarvis::specmap::trading_state::kActions;

  explicit TradingStateReplayer(const BehaviourFile& file)
      : limit_{static_cast<std::uint32_t>(file.constant("Limit"))},
        instrument_{perpetual(3).common} {}

  std::string start(const Step& init) {
    r::RiskConfig config;
    config.initial_state = state_named(init.var("base"));
    config.orders_per_10s = limit_;
    config.orders_per_minute = 0;
    config.margin_ratio_bps = 0;
    config.check_margin = false;
    risk_.emplace(config, 1, 1);
    return compare(init);
  }

  std::string step(const Step& s) {
    Diff diff;
    denies_inadmissible(s, diff);
    if (!diff.text().empty()) {
      return diff.text();
    }
    if (s.action == "Trigger") {
      static_cast<void>(risk().apply(trigger_named(s.args.at(0))));
    } else if (s.action == "Admit") {
      const std::string denied = decide(command_named(s.args.at(0)));
      if (!denied.empty()) {
        diff.note("the gates denied " + s.args.at(0) + " (" + denied + ")");
        return diff.text();
      }
    } else if (s.action == "Tick") {
      now_ = (now_ / kWindowNs + 1) * kWindowNs;
    } else {
      throw std::runtime_error("unknown TradingState action " + s.action);
    }
    return compare(s);
  }

private:
  r::RiskEngine& risk() {
    if (!risk_) {
      throw std::logic_error("TradingState replayer used before start");
    }
    return *risk_;
  }

  // The reason the kernel refuses a command of this kind now; empty when it goes through.
  // Orders pass check_order and modifies check_modify (both spend a unit of the rate window);
  // cancels bypass both gates and are refused only by the matrix.
  std::string decide(r::CommandKind kind) {
    if (kind == r::CommandKind::Cancel) {
      return r::allowed(risk().trading_state(), kind) ? std::string{} : std::string{"MATRIX"};
    }
    r::OrderCheck c;
    c.instrument = &instrument_;
    c.kind = kind;
    c.side = m::OrderSide::Buy;
    c.type = m::OrderType::Limit;
    c.quantity = quantity(100'000'000, 3);
    c.price = price(100 * static_cast<std::int64_t>(kScale), 1);
    c.reference = c.price;
    c.reduce_only = kind == r::CommandKind::Reduce;
    c.now = UnixNanos{now_};
    const bool modify = kind == r::CommandKind::Modify || kind == r::CommandKind::ModifyUp;
    return std::string{modify ? risk().check_modify(c) : risk().check_order(c)};
  }

  // Commands the spec did not admit in the state the step left must be refused there.
  void denies_inadmissible(const Step& s, Diff& diff) {
    const std::set<std::string> admissible = s.set("admissible");
    for (const std::string_view name : jarvis::specmap::trading_state::kCommands) {
      if (admissible.contains(std::string{name})) {
        continue;
      }
      if (decide(command_named(name)).empty()) {
        diff.note("the spec does not admit " + std::string{name} +
                  " before this step, but the gates let it through");
        return;
      }
    }
  }

  std::string compare(const Step& s) {
    Diff diff;
    const r::TradingStateMachine& machine = risk().state_machine();
    diff.expect("base", s.var("base"), std::string{r::spec_name(machine.base())});
    diff.expect("syncing", s.boolean("syncing"), machine.syncing());
    diff.expect("degraded", s.boolean("degraded"), machine.degraded());
    const std::uint32_t left = risk().limiter().remaining(UnixNanos{now_});
    diff.expect("used", s.integer("used"), static_cast<long long>(limit_ - left));
    return diff.text();
  }

  std::uint32_t limit_;
  m::InstrumentCommon instrument_;
  std::optional<r::RiskEngine> risk_;
  std::uint64_t now_ = 0;
};

// ---- Matching ----------------------------------------------------------------------------------

class MatchingReplayer {
public:
  static constexpr const auto& kActions = jarvis::specmap::matching::kActions;

  explicit MatchingReplayer(const BehaviourFile& /*file*/) : sim_{config()} {
    require(m::ClientOrderId::from("O-1", order_id_), "client order id");
    require(sim_.on_data(m::Event{perpetual(9)}, UnixNanos{++ts_}), "instrument");
  }

  std::string start(const Step& init) {
    qty_ = static_cast<std::uint64_t>(init.integer("qty"));
    post_only_ = init.boolean("postOnly");
    level_ = static_cast<std::uint64_t>(init.integer("level"));
    quote(std::nullopt);
    return compare(init);
  }

  std::string step(const Step& s) {
    if (s.action == "Rest") {
      submit();
    } else if (s.action == "Take") {
      quote(static_cast<std::uint64_t>(s.arg(0)));
      submit();
      quote(std::nullopt);
    } else if (s.action == "TradeAt") {
      trade(kPrice, static_cast<std::uint64_t>(s.arg(0)));
    } else if (s.action == "Through") {
      trade(kPrice - kTick, static_cast<std::uint64_t>(s.arg(0)));
    } else if (s.action == "Level") {
      level_ = static_cast<std::uint64_t>(s.arg(0));
      bid_at_price_ = true;
      quote(std::nullopt);
    } else if (s.action == "Gone") {
      level_ = 0;
      bid_at_price_ = false;
      quote(std::nullopt);
    } else if (s.action == "Cross") {
      quote(static_cast<std::uint64_t>(s.arg(0)));
      quote(std::nullopt);
    } else {
      throw std::runtime_error("unknown Matching action " + s.action);
    }
    return compare(s);
  }

private:
  static constexpr std::int64_t kPrice = 100'000'000'000; // 100.0
  static constexpr std::int64_t kTick = 100'000'000;      // 0.1

  static bt::SimConfig config() {
    bt::SimConfig c;
    c.fill_model = jarvis::specmap::matching::kFillModel;
    c.instruments = 2;
    c.orders = 8;
    return c;
  }

  // The market: the bid at p for `level_` (or one tick below p after Gone) and the ask one tick
  // above p, or at p for `offered` when set.
  void quote(std::optional<std::uint64_t> offered) {
    m::QuoteTick q;
    q.instrument_id = instrument_id();
    q.bid_price = price(bid_at_price_ ? kPrice : kPrice - kTick, 1);
    q.bid_size = quantity(bid_at_price_ ? level_ : 1, 9);
    q.ask_price = price(offered ? kPrice : kPrice + kTick, 1);
    q.ask_size = quantity(offered.value_or(1), 9);
    q.ts_event = UnixNanos{++ts_};
    q.ts_init = q.ts_event;
    data(m::Event{q});
  }

  void trade(std::int64_t px, std::uint64_t size) {
    m::TradeTick t;
    t.instrument_id = instrument_id();
    t.price = price(px, 1);
    t.size = quantity(size, 9);
    t.aggressor_side = m::AggressorSide::Sell;
    require(m::TradeId::from("T" + std::to_string(ts_), t.trade_id), "trade id");
    t.ts_event = UnixNanos{++ts_};
    t.ts_init = t.ts_event;
    data(m::Event{t});
  }

  void submit() {
    m::SubmitOrder c;
    c.client_order_id = order_id_;
    c.instrument_id = instrument_id();
    c.order_side = m::OrderSide::Buy;
    c.order_type = m::OrderType::Limit;
    c.quantity = quantity(qty_, 9);
    c.price = price(kPrice, 1);
    c.time_in_force = m::TimeInForce::Gtc;
    c.post_only = post_only_;
    sim_.clear_events();
    require(sim_.on_command(m::Output{c}, UnixNanos{++ts_}), "SimulatedExchange::on_command");
    submitted_ = true;
    collect();
  }

  void data(const m::Event& e) {
    sim_.clear_events();
    require(sim_.on_data(e, UnixNanos{++ts_}), "SimulatedExchange::on_data");
    collect();
  }

  void collect() {
    for (const m::OrderEvent& e : sim_.events()) {
      if (const auto* f = std::get_if<m::OrderFilled>(&e)) {
        filled_ += f->last_qty.raw();
        taker_ += f->liquidity_side == m::LiquiditySide::Taker ? f->last_qty.raw() : 0;
      }
    }
  }

  [[nodiscard]] std::optional<bt::SimOrder> resting() const {
    std::array<bt::SimOrder, 8> open{};
    const std::size_t n = sim_.open_orders(open);
    for (std::size_t i = 0; i < n && i < open.size(); ++i) {
      if (open[i].client_order_id == order_id_) {
        return open[i];
      }
    }
    return std::nullopt;
  }

  std::string compare(const Step& s) const {
    Diff diff;
    const std::optional<bt::SimOrder> order = resting();
    std::string phase = submitted_ ? "DONE" : "NEW";
    if (order) {
      phase = "RESTING";
    }
    diff.expect("phase", s.var("phase"), phase);
    diff.expect("filled", s.integer("filled"), static_cast<long long>(filled_));
    diff.expect("taker", s.integer("taker"), static_cast<long long>(taker_));
    diff.expect("level", s.integer("level"), static_cast<long long>(level_));
    if (order && phase == s.var("phase")) {
      diff.expect("ahead", s.integer("ahead"), static_cast<long long>(order->ahead_raw));
    }
    return diff.text();
  }

  bt::SimulatedExchange sim_;
  m::ClientOrderId order_id_;
  std::uint64_t ts_ = 0;
  std::uint64_t qty_ = 0;
  bool post_only_ = false;
  std::uint64_t level_ = 0;
  bool bid_at_price_ = true;
  bool submitted_ = false;
  std::uint64_t filled_ = 0;
  std::uint64_t taker_ = 0;
};

// ---- DepthSync ---------------------------------------------------------------------------------

class DepthSyncReplayer {
public:
  static constexpr const auto& kActions = jarvis::specmap::depth_sync::kActions;

  explicit DepthSyncReplayer(const BehaviourFile& file) : sync_{instrument()} {
    for (const std::string& p : jarvis::trace::to_set(file.constants.at("Prices"))) {
      price_set_.push_back(std::stoll(p));
    }
  }

  std::string start(const Step& init) { return compare(init); }

  std::string step(const Step& s) {
    const std::string& a = s.action;
    if (a == "Connect") {
      sync_.connected();
    } else if (a == "Disconnect") {
      require(sync_.disconnected(UnixNanos{++ts_}, out_), "DepthSync::disconnected");
    } else if (a == "Request") {
      request_ = sync_.snapshot_requested();
    } else if (a == "Receive") {
      jarvis::adapter::DepthDiff d;
      d.instrument_id = instrument();
      d.first_update_id = static_cast<std::uint64_t>(s.arg(0));
      d.final_update_id = static_cast<std::uint64_t>(s.arg(1));
      d.prev_final_update_id = static_cast<std::uint64_t>(s.arg(2));
      d.ts_event = UnixNanos{++ts_};
      d.ts_init = d.ts_event;
      const std::vector<jarvis::adapter::BookLevel> bids = levels(s.args.at(3), true);
      d.bids = bids;
      require(sync_.on_diff(d, out_), "DepthSync::on_diff");
    } else if (a == "Arrive") {
      jarvis::adapter::binance::DepthSnapshot snap;
      snap.last_update_id = static_cast<std::uint64_t>(s.arg(0));
      snap.ts_event = UnixNanos{++ts_};
      snap.ts_init = snap.ts_event;
      snap.bids = levels(s.args.at(1), false);
      require(sync_.on_snapshot(request_, snap, out_), "DepthSync::on_snapshot");
    } else if (a != "Update" && a != "Publish" && a != "Lose" && a != "Serve") {
      throw std::runtime_error("unknown DepthSync action " + a);
    }
    absorb();
    return compare(s);
  }

private:
  static constexpr std::int64_t kBase = 1000'000'000'000; // 1000.0
  static constexpr std::int64_t kTick = 100'000'000;      // 0.1
  static constexpr std::uint64_t kLot = 1'000'000;        // 0.001

  static m::InstrumentId instrument() {
    m::InstrumentId id;
    require(m::InstrumentId::parse("BTCUSDT-PERP.BINANCE", id), "instrument id");
    return id;
  }

  // Levels of a rendered spec book or change set; a snapshot leaves out empty levels.
  static std::vector<jarvis::adapter::BookLevel> levels(std::string_view text, bool keep_zero) {
    std::vector<jarvis::adapter::BookLevel> out;
    for (const auto& [p, q] : jarvis::trace::to_int_map(text)) {
      if (q == 0 && !keep_zero) {
        continue;
      }
      jarvis::adapter::BookLevel l;
      require(m::Price::from_raw(kBase + p * kTick, 1, l.price), "price");
      require(m::Quantity::from_raw(static_cast<std::uint64_t>(q) * kLot, 3, l.size), "size");
      out.push_back(l);
    }
    return out;
  }

  // The kernel's view: applies every emitted batch to a map of sizes.
  void absorb() {
    for (const m::Event& e : out_.events) {
      const auto& batch = std::get<m::OrderBookDeltas>(e);
      for (const m::OrderBookDelta& d : batch.deltas) {
        if (d.action == m::BookAction::Clear) {
          kernel_.clear();
          visible_ = m::has_flag(d.flags, m::RecordFlag::F_SNAPSHOT);
        } else if (d.action == m::BookAction::Delete) {
          kernel_.erase(tick_of(d.order.price));
        } else {
          kernel_[tick_of(d.order.price)] = static_cast<long long>(d.order.size.raw() / kLot);
        }
      }
    }
    out_.clear();
  }

  static long long tick_of(m::Price p) { return (p.raw() - kBase) / kTick; }

  // A spec book: every price of the model, zero when absent.
  [[nodiscard]] std::map<long long, long long>
  dense(const std::map<long long, long long>& sparse) const {
    std::map<long long, long long> out;
    for (const long long p : price_set_) {
      const auto it = sparse.find(p);
      out[p] = it == sparse.end() ? 0 : it->second;
    }
    return out;
  }

  [[nodiscard]] std::string compare(const Step& s) const {
    Diff diff;
    diff.expect("phase", s.var("phase"),
                std::string{jarvis::specmap::depth_sync::phase_name(sync_.phase())});
    std::map<long long, long long> local;
    for (const auto& [raw, l] : sync_.bids()) {
      local[(raw - kBase) / kTick] = static_cast<long long>(l.size.raw() / kLot);
    }
    const auto show = [](const std::map<long long, long long>& m) {
      std::string t;
      for (const auto& [k, v] : m) {
        t += (t.empty() ? "" : ",") + std::to_string(k) + ":" + std::to_string(v);
      }
      return "{" + t + "}";
    };
    diff.expect("local", show(dense(jarvis::trace::to_int_map(s.var("local")))),
                show(dense(local)));
    diff.expect("lastU", s.integer("lastU"), static_cast<long long>(sync_.last_update_id()));
    diff.expect("visible", s.boolean("visible"), visible_);
    diff.expect("visible (implementation)", s.boolean("visible"), sync_.visible());
    if (visible_) {
      diff.expect("out", show(dense(jarvis::trace::to_int_map(s.var("out")))),
                  show(dense(kernel_)));
    }
    return diff.text();
  }

  std::vector<long long> price_set_;
  jarvis::adapter::binance::DepthSync sync_;
  jarvis::adapter::CollectingEmitter out_;
  std::map<long long, long long> kernel_;
  bool visible_ = false;
  std::uint64_t request_ = 0;
  std::uint64_t ts_ = 0;
};

// ---- Reconciliation ----------------------------------------------------------------------------

// A rendered function over the spec's orders ("<a,b>" or "{1:a,2:b}") as text values by key.
std::map<long long, std::string> to_text_map(std::string_view text) {
  std::map<long long, std::string> out;
  const bool sequence = text.size() >= 2 && text.front() == '<' && text.back() == '>';
  if (!sequence && (text.size() < 2 || text.front() != '{' || text.back() != '}')) {
    throw std::runtime_error("not a function: " + std::string{text});
  }
  std::string_view body = text.substr(1, text.size() - 2);
  long long index = 1;
  while (!body.empty()) {
    const std::size_t comma = body.find(',');
    std::string_view item = body.substr(0, comma);
    if (sequence) {
      out[index++] = std::string{item};
    } else {
      const std::size_t colon = item.find(':');
      out[std::stoll(std::string{item.substr(0, colon)})] = std::string{item.substr(colon + 1)};
    }
    body = comma == std::string_view::npos ? std::string_view{} : body.substr(comma + 1);
  }
  return out;
}

// Submits one order of `max_fill` units per spec order when it starts; records, as the spec's
// `seen` renders them, the trades it receives. Writes through pointers to the replayer's state.
// NOLINTBEGIN(readability-make-member-function-const)
struct ReconciliationTrader {
  std::size_t orders = 0;
  std::uint64_t max_fill = 0;
  std::vector<m::ClientOrderId>* ids = nullptr;
  std::set<std::string>* seen = nullptr;

  Status on_start(st::Context& ctx) {
    for (std::size_t i = 0; i < orders; ++i) {
      m::ClientOrderId id;
      const Status s =
          ctx.submit(ctx.limit(instrument_id(), m::OrderSide::Buy, quantity(max_fill * kScale, 0),
                               price(100 * static_cast<std::int64_t>(kScale), 1)),
                     id);
      if (!jarvis::core::ok(s)) {
        return s;
      }
      ids->push_back(id);
    }
    return Status::Ok;
  }
  Status on_order_event(st::Context& /*ctx*/, const m::OrderEvent& e) {
    if (const auto* f = std::get_if<m::OrderFilled>(&e)) {
      std::string trade{f->trade_id.view()};
      trade[trade.find('-')] = ',';
      seen->insert("<" + trade + ">");
    }
    return Status::Ok;
  }
};
// NOLINTEND(readability-make-member-function-const)

class ReconciliationReplayer {
public:
  static constexpr const auto& kActions = jarvis::specmap::reconciliation::kActions;

  explicit ReconciliationReplayer(const BehaviourFile& file)
      : max_fill_{static_cast<std::uint64_t>(file.constant("MaxFill"))},
        orders_{jarvis::trace::to_set(file.constants.at("Orders")).size()},
        set_{ReconciliationTrader{orders_, max_fill_, &ids_, &seen_}}, engine_{config(), set_},
        venue_(orders_ + 1) {}

  // The spec's Init: the node ran, its strategy submitted the orders, and the user stream is
  // down (Disconnected, halted).
  std::string start(const Step& init) {
    input(m::Event{perpetual(0)});
    for (const auto reason : {m::LifecycleReason::Configured, m::LifecycleReason::RunRequested,
                              m::LifecycleReason::Started, m::LifecycleReason::Synced}) {
      transition(reason);
    }
    if (ids_.size() != orders_) {
      throw std::runtime_error("the strategy did not submit every order");
    }
    input(stream(false));
    gate();
    return compare(init);
  }

  std::string step(const Step& s) {
    const std::string& a = s.action;
    if (a == "Open" || a == "Fill" || a == "Cancel") {
      venue_change(a, static_cast<std::size_t>(s.arg(0)));
    } else if (a == "Disconnect" || a == "Connect") {
      input(stream(a == "Connect"));
    } else if (a == "Deliver") {
      input(message(s));
    } else if (a == "SnapshotTaken") {
      const std::string diff = take_snapshot(s);
      if (!diff.empty()) {
        return diff;
      }
    } else if (a == "Reconcile") {
      input(snapshot_event());
    } else if (a != "RequestSnapshot") {
      throw std::runtime_error("unknown Reconciliation action " + a);
    }
    gate();
    return compare(s);
  }

private:
  // The venue as the driver saw it change (the spec's xst and xf, with the times).
  struct VenueOrder {
    std::string st = "none";
    std::uint64_t fills = 0;
    std::uint64_t ts_open = 0;
    std::uint64_t ts_last = 0;
    std::vector<std::uint64_t> fill_ts;
  };

  static st::KernelConfig config() {
    st::KernelConfig c;
    c.instruments = 4;
    c.strategies = 1;
    c.timers = 4;
    c.features = 1;
    c.bar_types = 1;
    c.buffers = 4;
    c.book_window_levels = 64;
    c.book_overflow_levels = 16;
    c.trading.orders = 16;
    c.trading.trades = 64;
    c.trading.risk.orders_per_10s = 0;
    c.trading.risk.orders_per_minute = 0;
    c.trading.reconcile.lost_grace = jarvis::core::DurationNanos{~0ULL / 4}; // venue "none"
    return c;
  }

  static m::Event stream(bool up) {
    m::ConnectionStatus c;
    c.kind = m::ConnectionKind::UserStream;
    c.up = up;
    return m::Event{c};
  }

  static m::VenueOrderId venue_id(std::size_t o) {
    m::VenueOrderId id;
    require(m::VenueOrderId::from("v" + std::to_string(o), id), "venue order id");
    return id;
  }

  void input(m::Event event) {
    const UnixNanos now{++now_};
    std::visit(
        [now](auto& e) {
          if constexpr (requires { e.ts_init; }) {
            e.ts_init = now;
          } else if constexpr (requires { e.header.ts_init; }) {
            e.header.ts_init = now;
          }
        },
        event);
    require(engine_.step(jarvis::core::EventKey{now, 0, now_}, event), "Engine::step");
    engine_.clear_outputs();
  }

  void transition(m::LifecycleReason reason) {
    m::NodeLifecycle event;
    require(lifecycle_.apply(reason, UnixNanos{now_ + 1}, event), "Lifecycle::apply");
    input(m::Event{event});
  }

  // The driver's sync gate, as backtest::Driver applies it with await_sync.
  void gate() {
    while (const std::optional<m::LifecycleReason> move = jarvis::engine::sync_move(
               lifecycle_.state(), engine_.kernel().trading.reconciler.phase(), true)) {
      transition(*move);
    }
  }

  void venue_change(const std::string& action, std::size_t o) {
    VenueOrder& v = venue_.at(o);
    ++xtime_;
    v.ts_last = xtime_;
    if (action == "Open") {
      v.st = "open";
      v.ts_open = xtime_;
    } else if (action == "Fill") {
      ++v.fills;
      v.fill_ts.push_back(xtime_);
      v.st = v.fills == max_fill_ ? "done" : "open";
    } else {
      v.st = "done";
    }
  }

  [[nodiscard]] m::OrderEventHeader header(std::size_t o, std::uint64_t ts_event) const {
    m::OrderEventHeader h;
    h.instrument_id = instrument_id();
    h.client_order_id = ids_.at(o - 1);
    h.ts_event = UnixNanos{ts_event};
    return h;
  }

  [[nodiscard]] m::Event message(const Step& s) const {
    const auto o = static_cast<std::size_t>(s.arg(0));
    const long long n = s.arg(1);
    const auto t = static_cast<std::uint64_t>(s.arg(4));
    if (n > 0) {
      m::OrderFilled e;
      e.header = header(o, t);
      e.venue_order_id = venue_id(o);
      require(m::TradeId::from(std::to_string(o) + "-" + std::to_string(n), e.trade_id),
              "trade id");
      e.order_side = m::OrderSide::Buy;
      e.order_type = m::OrderType::Limit;
      e.last_qty = quantity(kScale, 0);
      e.last_px = price(100 * static_cast<std::int64_t>(kScale), 1);
      e.liquidity_side = m::LiquiditySide::Maker;
      return m::Event{e};
    }
    if (s.args.at(2) == "open") {
      m::OrderAccepted e;
      e.header = header(o, t);
      e.venue_order_id = venue_id(o);
      return m::Event{e};
    }
    m::OrderCanceled e;
    e.header = header(o, t);
    e.venue_order_id = venue_id(o);
    return m::Event{e};
  }

  // The venue at its time T_s, as the adapter would report it; checked against the spec's.
  std::string take_snapshot(const Step& s) {
    Diff diff;
    diff.expect("snapshot T_s", s.arg(0), static_cast<long long>(xtime_));
    const std::map<long long, std::string> st = to_text_map(s.args.at(1));
    const std::map<long long, long long> f = jarvis::trace::to_int_map(s.args.at(2));
    orders_reports_.clear();
    fill_reports_.clear();
    position_reports_.clear();
    std::uint64_t total = 0;
    for (std::size_t o = 1; o <= orders_; ++o) {
      const VenueOrder& v = venue_.at(o);
      diff.expect("snapshot st[" + std::to_string(o) + "]", st.at(static_cast<long long>(o)), v.st);
      diff.expect("snapshot f[" + std::to_string(o) + "]", f.at(static_cast<long long>(o)),
                  static_cast<long long>(v.fills));
      total += v.fills;
      if (v.st == "none") {
        continue;
      }
      m::OrderStatusReport r;
      r.instrument_id = instrument_id();
      r.client_order_id = ids_.at(o - 1);
      r.venue_order_id = venue_id(o);
      r.order_status = m::OrderStatus::Accepted;
      if (v.st == "open" && v.fills > 0) {
        r.order_status = m::OrderStatus::PartiallyFilled;
      } else if (v.st == "done") {
        r.order_status = v.fills == max_fill_ ? m::OrderStatus::Filled : m::OrderStatus::Canceled;
      }
      r.quantity = quantity(max_fill_ * kScale, 0);
      r.filled_qty = quantity(v.fills * kScale, 0);
      r.price = price(100 * static_cast<std::int64_t>(kScale), 1);
      r.ts_accepted = UnixNanos{v.ts_open};
      r.ts_last = UnixNanos{v.ts_last};
      orders_reports_.push_back(r);
      for (std::uint64_t n = 1; n <= v.fills; ++n) {
        m::FillReport fr;
        fr.instrument_id = instrument_id();
        fr.venue_order_id = venue_id(o);
        require(m::TradeId::from(std::to_string(o) + "-" + std::to_string(n), fr.trade_id),
                "trade id");
        fr.last_qty = quantity(kScale, 0);
        fr.last_px = price(100 * static_cast<std::int64_t>(kScale), 1);
        require(m::Money::from_raw(0, perpetual(0).common.quote_currency, fr.commission),
                "commission");
        fr.liquidity_side = m::LiquiditySide::Maker;
        fr.client_order_id = ids_.at(o - 1);
        fr.ts_event = UnixNanos{v.fill_ts.at(n - 1)};
        fill_reports_.push_back(fr);
      }
    }
    if (total > 0) {
      m::PositionStatusReport p;
      p.instrument_id = instrument_id();
      p.position_side = m::PositionSide::Long;
      p.quantity = quantity(total * kScale, 0);
      p.avg_px_open = price(100 * static_cast<std::int64_t>(kScale), 1);
      position_reports_.push_back(p);
    }
    snapshot_ts_ = xtime_;
    return diff.text();
  }

  [[nodiscard]] m::Event snapshot_event() const {
    m::VenueSnapshot v;
    v.ts_snapshot = UnixNanos{snapshot_ts_};
    v.orders = orders_reports_;
    v.fills = fill_reports_;
    v.positions = position_reports_;
    return m::Event{v};
  }

  [[nodiscard]] std::string compare(const Step& s) const {
    namespace map = jarvis::specmap::reconciliation;
    Diff diff;
    const st::Trading& t = engine_.kernel().trading;
    const std::string_view phase = map::kernel_phase(s.var("phase"));
    diff.expect("phase", phase, map::phase_name(t.reconciler.phase()));
    diff.expect("node Running", phase == "Synced", lifecycle_.state() == m::NodeState::Running);
    const m::TradingState trading = t.risk.trading_state();
    diff.expect("trading", s.var("trading"),
                std::string{trading == m::TradingState::Active ? "Active" : "Halted"});
    diff.expect("trading (not REDUCING)", true, trading != m::TradingState::Reducing);
    const std::map<long long, std::string> lst = to_text_map(s.var("lst"));
    const std::map<long long, long long> lf = jarvis::trace::to_int_map(s.var("lf"));
    for (std::size_t o = 1; o <= orders_; ++o) {
      const ex::OrderRecord& r = t.oms.at(t.oms.find(ids_.at(o - 1)));
      const std::string key = "[" + std::to_string(o) + "]";
      diff.expect("lst" + key, lst.at(static_cast<long long>(o)),
                  std::string{map::status_name(r.state.status())});
      diff.expect("lf" + key, lf.at(static_cast<long long>(o)),
                  static_cast<long long>(r.state.filled().raw() / kScale));
    }
    diff.expect("lpos (strategy)", s.integer("lpos"),
                static_cast<long long>(t.portfolio.position(0, 0).signed_raw()) /
                    static_cast<long long>(kScale));
    diff.expect("lpos (venue)", s.integer("lpos"),
                static_cast<long long>(t.portfolio.venue(0).signed_raw()) /
                    static_cast<long long>(kScale));
    std::string seen;
    for (const std::string& trade : seen_) {
      seen += (seen.empty() ? "" : ",") + trade;
    }
    diff.expect("seen", s.var("seen"), "{" + seen + "}");
    return diff.text();
  }

  std::uint64_t max_fill_;
  std::size_t orders_;
  std::vector<m::ClientOrderId> ids_;
  std::set<std::string> seen_;
  st::StaticStrategySet<ReconciliationTrader> set_;
  jarvis::engine::Engine<st::StaticStrategySet<ReconciliationTrader>> engine_;
  jarvis::engine::Lifecycle lifecycle_;
  std::vector<VenueOrder> venue_; // by spec order, from 1
  std::uint64_t xtime_ = 0;
  std::uint64_t now_ = 0;
  std::uint64_t snapshot_ts_ = 0;
  std::vector<m::OrderStatusReport> orders_reports_;
  std::vector<m::FillReport> fill_reports_;
  std::vector<m::PositionStatusReport> position_reports_;
};

// ---- driver ------------------------------------------------------------------------------------

std::string describe(const Step& s) {
  std::string out = s.action;
  for (const std::string& a : s.args) {
    out += " " + a;
  }
  return out;
}

template <typename Replayer> bool replay(const BehaviourFile& file, const std::string& path) {
  std::map<std::string, std::size_t, std::less<>> seen;
  std::size_t steps = 0;
  for (const jarvis::trace::Behaviour& b : file.behaviours) {
    if (b.steps.empty() || b.steps.front().action != "Init") {
      std::cerr << path << ": behaviour " << b.number << " does not start with Init\n";
      return false;
    }
    Replayer replayer{file};
    for (std::size_t i = 0; i < b.steps.size(); ++i) {
      const Step& s = b.steps[i];
      const std::string diff = i == 0 ? replayer.start(s) : replayer.step(s);
      if (!diff.empty()) {
        std::cerr << path << ":" << s.line << ": " << file.spec << " behaviour " << b.number
                  << " step " << i << " (" << describe(s) << ") diverges:\n"
                  << diff;
        return false;
      }
      ++seen[s.action];
      steps += i == 0 ? 0 : 1;
    }
  }
  bool ok = true;
  for (const std::string_view action : Replayer::kActions) {
    if (!seen.contains(action)) {
      std::cerr << path << ": action " << action
                << " never appears; the behaviours module may lack a disjunct, or the file is "
                   "too small\n";
      ok = false;
    }
  }
  std::cout << "trace_driver: " << file.spec << ": " << file.behaviours.size() << " behaviours, "
            << steps << " steps matched\n";
  return ok;
}

bool run(const std::string& path) {
  const BehaviourFile file = jarvis::trace::read_behaviour_file(path);
  if (file.spec == "OrderLifecycle") {
    return replay<OrderLifecycleReplayer>(file, path);
  }
  if (file.spec == "TradingState") {
    return replay<TradingStateReplayer>(file, path);
  }
  if (file.spec == "Matching") {
    return replay<MatchingReplayer>(file, path);
  }
  if (file.spec == "DepthSync") {
    return replay<DepthSyncReplayer>(file, path);
  }
  if (file.spec == "Reconciliation") {
    return replay<ReconciliationReplayer>(file, path);
  }
  std::cerr << path << ": no trace driver for spec " << file.spec << "\n";
  return false;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: trace_driver <behaviours.txt>...\n";
    return 2;
  }
  bool ok = true;
  try {
    for (int i = 1; i < argc; ++i) {
      ok = run(argv[i]) && ok;
    }
  } catch (const std::exception& e) {
    std::cerr << "trace_driver: " << e.what() << "\n";
    return 2;
  }
  return ok ? 0 : 1;
}
