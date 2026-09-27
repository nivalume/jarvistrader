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
#include "jarvis/backtest/matching/sim_exchange.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/execution/execution_engine.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/risk/gates.hpp"
#include "jarvis/risk/trading_state.hpp"
#include "specs/map/matching_actions.hpp"
#include "specs/map/order_lifecycle_actions.hpp"
#include "specs/map/trading_state_actions.hpp"

namespace {

namespace bt = jarvis::backtest;
namespace ex = jarvis::execution;
namespace m = jarvis::model;
namespace r = jarvis::risk;
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
