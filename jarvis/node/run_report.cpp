#include "jarvis/node/run_report.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <variant>

#include "jarvis/core/time.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/backtest_node.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/order_replay.hpp"
#include "jarvis/node/run_dir.hpp"
#include "jarvis/portfolio/portfolio.hpp"

namespace jarvis::node {

namespace {

using core::Status;
namespace ex = execution;

template <typename T> std::string text(const T& value) {
  std::array<char, 96> buffer{};
  std::size_t n = 0;
  if (!core::ok(value.format(std::span<char>{buffer}, n))) {
    return "?";
  }
  return std::string{buffer.data(), n};
}

std::string name_of(const model::InstrumentId& id) {
  return std::string{id.symbol.view()} + "." + std::string{id.venue.view()};
}

std::string rfc3339(core::UnixNanos t) {
  std::array<char, core::kRfc3339MaxLength> buffer{};
  std::size_t n = 0;
  if (!core::ok(core::format_rfc3339(t, buffer, n))) {
    return "?";
  }
  return std::string{buffer.data(), n};
}

std::string millis(std::uint64_t ns) {
  std::ostringstream out;
  out << ns / 1'000'000 << '.' << (ns / 100'000) % 10 << (ns / 10'000) % 10 << (ns / 1'000) % 10
      << " ms";
  return out.str();
}

std::string_view fill_model_name(FillModel m) {
  return m == FillModel::QueuePosition ? "queue_position" : "top_of_book";
}

std::string_view stp_name(SelfTradePrevention s) {
  switch (s) {
  case SelfTradePrevention::None:
    return "none";
  case SelfTradePrevention::ExpireTaker:
    return "expire_taker";
  case SelfTradePrevention::ExpireMaker:
    return "expire_maker";
  case SelfTradePrevention::ExpireBoth:
    return "expire_both";
  }
  return "none";
}

std::string_view env_name(Env e) {
  switch (e) {
  case Env::Backtest:
    return "backtest";
  case Env::Sandbox:
    return "sandbox";
  case Env::Live:
    return "live";
  }
  return "backtest";
}

// Truncated to the currency's grid, as strategy::Trading reports amounts.
model::Money money(std::int64_t raw, const model::Currency& currency) {
  model::Money m;
  if (!core::ok(model::Money::from_raw_truncated(raw, currency, m))) {
    static_cast<void>(model::Money::from_raw(0, currency, m));
  }
  return m;
}

void labels(const NodeConfig& config, RunReport& out) {
  out.node_id = config.node.id;
  out.env = std::string{env_name(config.node.env)};
  out.seed = config.node.seed;
  out.catalog = config.data.catalog;
  if (config.data.range) {
    out.range = rfc3339(config.data.range->start) + " to " + rfc3339(config.data.range->end);
  }
  bool depth = false;
  bool quotes = false;
  bool trades = false;
  for (const DataStream& group : config.data.streams) {
    for (const std::string& stream : group.streams) {
      if (std::find(out.streams.begin(), out.streams.end(), stream) == out.streams.end()) {
        out.streams.push_back(stream);
      }
      depth = depth || stream.starts_with("depth");
      quotes = quotes || stream == "bookTicker";
      trades = trades || stream == "aggTrade" || stream == "trade";
    }
    for (const model::InstrumentId& id : group.instruments) {
      const std::string name = name_of(id);
      if (std::find(out.instruments.begin(), out.instruments.end(), name) ==
          out.instruments.end()) {
        out.instruments.push_back(name);
      }
    }
  }
  if (depth) {
    out.book = "L2 (depth)";
  } else if (quotes) {
    out.book = "L1 (bookTicker)";
  } else {
    out.book = "no book";
  }
  if (trades) {
    out.book += " + trades (aggTrade)";
  }
  const std::optional<SimSection>& sim =
      config.venues.empty() ? std::optional<SimSection>{} : config.venues.front().sim;
  if (!sim) {
    out.venue = "none (data only)";
    return;
  }
  out.fill_model = std::string{fill_model_name(sim->fill_model)};
  out.fee_schedule = sim->fee_schedule.empty() ? "binance_usdm_vip0" : sim->fee_schedule;
  out.venue = "simulated; fill model " + out.fill_model + "; latency feed " +
              millis(sim->latency.feed_ns) + ", out " + millis(sim->latency.out_ns) + ", in " +
              millis(sim->latency.in_ns) + ", jitter " + millis(sim->latency.jitter_ns) +
              "; fees " + out.fee_schedule + "; STP " + std::string{stp_name(sim->stp)};
}

class Reporter {
public:
  Reporter(const NodeConfig& config, std::size_t orders, std::size_t fills)
      : config_{config},
        portfolio_{portfolio::PortfolioConfig{config.node.capacity.instruments,
                                              config.node.capacity.strategies, 16,
                                              kernel_config(config).trading.margin}},
        replay_{orders, fills} {}

  void on_input(const model::wire::RecordHeader& header, const model::Event& event) {
    if (first_ts_ == 0) {
      first_ts_ = header.ts.value();
    }
    last_ts_ = header.ts.value();
    std::visit([this](const auto& e) { this->input(e); }, event);
  }

  void on_output(const model::Output& output) {
    const ReplayOutcome r = replay_.on_output(output);
    if (const auto* denied = std::get_if<model::OrderDenied>(&output)) {
      ++counts(std::string{denied->header.strategy_id.view()}).denied;
      return;
    }
    if (!r.known) {
      return;
    }
    OrderCounts& c = counts(strategy_name(replay_.oms().at(r.index).strategy));
    if (std::holds_alternative<model::SubmitOrder>(output)) {
      ++c.submitted;
    } else if (std::holds_alternative<model::ModifyOrder>(output)) {
      ++c.modifies;
    } else if (std::holds_alternative<model::CancelOrder>(output)) {
      ++c.cancels;
    }
  }

  void finish(RunReport& out) {
    out.starting_balances = start_;
    for (const model::Money& m : start_) {
      std::int64_t wallet = 0;
      if (portfolio_.wallet(m.currency(), wallet)) {
        out.ending_balances.push_back(money(wallet, m.currency()));
      }
    }
    for (const auto& [key, acc] : accs_) {
      out.rows.push_back(row(key.first, key.second, acc));
    }
    for (const auto& [name, c] : orders_) {
      out.orders.emplace_back(name, c);
    }
    out.first_ts = first_ts_;
    out.last_ts = last_ts_;
  }

private:
  struct Acc {
    std::uint64_t fills = 0;
    std::uint64_t maker = 0;
    std::uint64_t taker = 0;
    std::uint64_t bought = 0;
    std::uint64_t sold = 0;
    std::int64_t notional = 0;
  };
  struct Market {
    model::InstrumentId id;
    std::optional<model::Instrument> definition;
    std::optional<model::Price> mid;
    bool mark = false;
    bool trade = false;
  };

  template <typename T> void input(const T& e) {
    if constexpr (std::is_same_v<T, model::CurrencyPair> ||
                  std::is_same_v<T, model::CryptoPerpetual> ||
                  std::is_same_v<T, model::CryptoFuture>) {
      markets_[slot(e.common.id)].definition = model::Instrument{e};
    } else if constexpr (std::is_same_v<T, model::AccountState>) {
      if (!have_start_) {
        for (const model::AccountBalance& b : e.balances) {
          start_.push_back(b.total);
        }
        have_start_ = true;
      }
      static_cast<void>(portfolio_.set_account(e));
    } else if constexpr (std::is_same_v<T, model::TradeTick>) {
      const std::uint32_t s = slot(e.instrument_id);
      portfolio_.note_trade(s, e.price);
      markets_[s].trade = true;
    } else if constexpr (std::is_same_v<T, model::QuoteTick>) {
      model::Price mid;
      if (core::ok(model::Price::from_raw((e.bid_price.raw() + e.ask_price.raw()) / 2,
                                          static_cast<std::uint8_t>(model::kFixedPrecision),
                                          mid))) {
        markets_[slot(e.instrument_id)].mid = mid;
      }
    } else if constexpr (std::is_same_v<T, model::MarkPriceUpdate>) {
      const std::uint32_t s = slot(e.instrument_id);
      portfolio_.set_mark(s, e.value);
      markets_[s].mark = true;
    } else if constexpr (std::is_same_v<T, model::FundingRateUpdate>) {
      const std::uint32_t s = slot(e.instrument_id);
      const std::optional<model::Instrument>& def = markets_[s].definition;
      if (def) {
        std::span<const portfolio::FundingShare> shares;
        static_cast<void>(portfolio_.on_funding(*def, s, e, shares));
      }
    } else if constexpr (kIsOrderEvent<T>) {
      order_event(model::OrderEvent{e});
    }
  }

  void order_event(const model::OrderEvent& event) {
    const ReplayOutcome r = replay_.on_event(event);
    if (!r.known) {
      return;
    }
    const ex::OrderRecord& o = replay_.oms().at(r.index);
    OrderCounts& c = counts(strategy_name(o.strategy));
    if (!r.applied) {
      ++c.refused;
      return;
    }
    switch (r.kind) {
    case ex::OrderEventKind::Accepted:
      ++c.accepted;
      break;
    case ex::OrderEventKind::Rejected:
      ++c.rejected;
      break;
    case ex::OrderEventKind::Canceled:
      ++c.canceled;
      break;
    case ex::OrderEventKind::Expired:
      ++c.expired;
      break;
    case ex::OrderEventKind::ModifyRejected:
      ++c.modify_rejected;
      break;
    case ex::OrderEventKind::CancelRejected:
      ++c.cancel_rejected;
      break;
    case ex::OrderEventKind::Filled:
      c.filled += o.state.status() == model::OrderStatus::Filled ? 1U : 0U;
      book(o, event);
      break;
    case ex::OrderEventKind::FillVoided:
      book(o, event);
      break;
    case ex::OrderEventKind::Denied:
    case ex::OrderEventKind::Emulated:
    case ex::OrderEventKind::Released:
    case ex::OrderEventKind::Submitted:
    case ex::OrderEventKind::Triggered:
    case ex::OrderEventKind::PendingUpdate:
    case ex::OrderEventKind::PendingCancel:
    case ex::OrderEventKind::Updated:
      break;
    }
  }

  // As strategy::Trading::book: a fill, or a void as the opposite fill with its fee refunded.
  void book(const ex::OrderRecord& o, const model::OrderEvent& event) {
    const std::uint32_t s = slot(o.instrument_id);
    const std::optional<model::Instrument>& def = markets_[s].definition;
    if (!def) {
      return;
    }
    model::OrderSide side = o.side;
    model::Quantity qty;
    model::Price px;
    std::optional<model::Money> commission;
    core::UnixNanos ts;
    Acc& acc = accs_[{o.strategy, s}];
    if (const auto* f = std::get_if<model::OrderFilled>(&event)) {
      qty = f->last_qty;
      px = f->last_px;
      commission = f->commission;
      ts = f->header.ts_event;
      ++acc.fills;
      acc.maker += f->liquidity_side == model::LiquiditySide::Maker ? 1U : 0U;
      acc.taker += f->liquidity_side == model::LiquiditySide::Taker ? 1U : 0U;
      (side == model::OrderSide::Buy ? acc.bought : acc.sold) += qty.raw();
      model::Money n;
      if (core::ok(model::notional_value(model::common(*def), qty, px, n))) {
        acc.notional += n.raw();
      }
    } else if (const auto* v = std::get_if<model::OrderFillVoided>(&event)) {
      side = o.side == model::OrderSide::Buy ? model::OrderSide::Sell : model::OrderSide::Buy;
      qty = v->voided_qty;
      px = v->last_px;
      if (v->commission_voided) {
        commission = money(-v->commission_voided->raw(), v->commission_voided->currency());
      }
      ts = v->header.ts_event;
      (o.side == model::OrderSide::Buy ? acc.bought : acc.sold) -= qty.raw();
    } else {
      return;
    }
    portfolio::FillOutcome outcome;
    static_cast<void>(portfolio_.on_fill(*def, s, o.strategy, side, qty, px, commission,
                                         o.client_order_id, ts, outcome));
  }

  ReportRow row(portfolio::StrategyIndex strategy, std::uint32_t s, const Acc& acc) {
    ReportRow r;
    r.strategy = strategy_name(strategy);
    Market& m = markets_[s];
    r.instrument = name_of(m.id);
    r.fills = acc.fills;
    r.maker_fills = acc.maker;
    r.taker_fills = acc.taker;
    if (!m.definition) {
      return r;
    }
    const model::InstrumentCommon& c = model::common(*m.definition);
    const model::Currency& ccy = c.settlement_currency;
    static_cast<void>(model::Quantity::from_raw(acc.bought, c.size_precision, r.bought));
    static_cast<void>(model::Quantity::from_raw(acc.sold, c.size_precision, r.sold));
    r.notional = money(acc.notional, ccy);
    r.valuation_source = "none";
    if (m.mark) {
      r.valuation_source = "mark";
    } else if (m.trade) {
      r.valuation_source = "last trade";
    } else if (m.mid) {
      portfolio_.note_trade(s, *m.mid);
      r.valuation_source = "last quote mid";
    }
    r.valuation = portfolio_.valuation(s);
    const portfolio::NettingPosition& p = portfolio_.position(strategy, s);
    std::int64_t unrealized = 0;
    static_cast<void>(portfolio_.unrealized(c, s, p, unrealized));
    r.realized = money(p.total_realized_raw(), ccy);
    r.commission = money(p.total_commission_raw(), ccy);
    r.funding = money(p.total_funding_raw(), ccy);
    r.unrealized = money(unrealized, ccy);
    r.net = money(p.total_realized_raw() - p.total_commission_raw() + p.total_funding_raw() +
                      unrealized,
                  ccy);
    static_cast<void>(model::Decimal::from_raw(p.signed_raw(), c.size_precision, r.position));
    model::Price avg;
    if (p.is_open() && p.avg_px_open(avg)) {
      r.avg_px_open = avg;
    }
    return r;
  }

  std::uint32_t slot(const model::InstrumentId& id) {
    for (std::uint32_t i = 0; i < markets_.size(); ++i) {
      if (markets_[i].id == id) {
        return i;
      }
    }
    markets_.push_back(Market{id, std::nullopt, std::nullopt, false, false});
    return static_cast<std::uint32_t>(markets_.size() - 1);
  }

  [[nodiscard]] std::string strategy_name(portfolio::StrategyIndex s) const {
    if (s < config_.strategies.size()) {
      return config_.strategies[s].id;
    }
    return "strategy-" + std::to_string(s + 1);
  }

  OrderCounts& counts(const std::string& strategy) { return orders_[strategy]; }

  const NodeConfig& config_;
  portfolio::Portfolio portfolio_;
  OrderReplay replay_;
  std::vector<Market> markets_;
  std::map<std::pair<portfolio::StrategyIndex, std::uint32_t>, Acc> accs_;
  std::map<std::string, OrderCounts> orders_;
  std::vector<model::Money> start_;
  bool have_start_ = false;
  std::uint64_t first_ts_ = 0;
  std::uint64_t last_ts_ = 0;
};

} // namespace

Status build_run_report(const std::string& directory, RunReport& out, std::string& error) {
  out = RunReport{};
  out.directory = directory;
  NodeConfig config;
  Status s = load_run_config(directory, config, error);
  if (!core::ok(s)) {
    return s;
  }
  labels(config, out);
  std::size_t orders = 0;
  std::size_t fills = 0;
  s = OrderReplay::count(directory, orders, fills);
  if (!core::ok(s)) {
    error = "cannot read the run log in " + directory;
    return s;
  }
  Reporter reporter{config, orders, fills};
  EventLogReader reader;
  s = reader.open(directory);
  model::wire::RecordView record;
  while (core::ok(s) && core::ok(s = reader.next(record))) {
    if (record.header.kind >= model::wire::kFirstOutputKind) {
      model::Output output;
      s = EventLogReader::decode_output(record, output);
      if (core::ok(s)) {
        reporter.on_output(output);
      }
      continue;
    }
    model::Event event;
    s = reader.decode(record, event);
    if (core::ok(s)) {
      reporter.on_input(record.header, event);
    }
  }
  if (s != Status::EndOfStream) {
    error = "cannot decode the run log in " + directory;
    return s;
  }
  reporter.finish(out);
  return Status::Ok;
}

std::string report_text(const RunReport& r) {
  std::ostringstream out;
  const auto join = [](const std::vector<std::string>& items) {
    std::string s;
    for (const std::string& item : items) {
      s += (s.empty() ? "" : ", ") + item;
    }
    return s;
  };
  const auto balances = [](const std::vector<model::Money>& items) {
    std::string s;
    for (const model::Money& m : items) {
      s += (s.empty() ? "" : ", ") + text(m);
    }
    return s.empty() ? std::string{"none"} : s;
  };
  out << "run: " << r.directory << "\n"
      << "node: " << r.node_id << " (" << r.env << ", seed " << r.seed << ")\n"
      << "data: " << join(r.streams) << " for " << join(r.instruments) << " from " << r.catalog
      << (r.range.empty() ? "" : ", " + r.range) << "\n"
      << "fills simulated against: " << r.book << "\n"
      << "venue: " << r.venue << "\n"
      << "account: " << balances(r.starting_balances) << " -> " << balances(r.ending_balances)
      << "\n";
  for (const ReportRow& row : r.rows) {
    out << "strategy " << row.strategy << " on " << row.instrument << ":\n"
        << "  fills " << row.fills << " (maker " << row.maker_fills << ", taker " << row.taker_fills
        << "), bought " << text(row.bought) << ", sold " << text(row.sold) << ", notional "
        << text(row.notional) << "\n"
        << "  realized " << text(row.realized) << ", commission " << text(row.commission)
        << ", funding " << text(row.funding) << "\n"
        << "  position " << text(row.position);
    if (row.avg_px_open) {
      out << " at " << text(*row.avg_px_open);
    }
    out << ", unrealized " << text(row.unrealized);
    if (row.valuation) {
      out << " at " << text(*row.valuation) << " (" << row.valuation_source << ")";
    }
    out << "\n  net " << text(row.net) << "\n";
  }
  for (const auto& [strategy, c] : r.orders) {
    out << "orders of " << strategy << ": submitted " << c.submitted << ", denied " << c.denied
        << ", accepted " << c.accepted << ", rejected " << c.rejected << ", filled " << c.filled
        << ", canceled " << c.canceled << ", expired " << c.expired << "; modifies " << c.modifies
        << " (" << c.modify_rejected << " rejected), cancels " << c.cancels << " ("
        << c.cancel_rejected << " rejected); refused events " << c.refused << "\n";
  }
  return out.str();
}

} // namespace jarvis::node
