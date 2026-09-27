#include "jarvis/node/backtest_node.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/cost/fees.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instrument_table.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/node/event_text.hpp"
#include "jarvis/node/replay.hpp"
#include "jarvis/portfolio/margin.hpp"
#include "jarvis/risk/gates.hpp"

namespace jarvis::node {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

std::string upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return text;
}

} // namespace

strategy::KernelConfig kernel_config(const NodeConfig& config) {
  strategy::KernelConfig k;
  k.instruments = config.node.capacity.instruments;
  k.strategies = config.node.capacity.strategies;
  k.timers = config.node.capacity.timers;
  k.batch = config.node.capacity.batch;
  k.seed = config.node.seed;
  // Orders, their fill records (16 per order on average) and the identities they carry: the
  // ClientOrderId prefix is the node id, the trader "<NODE>-001", the account "<VENUE>-001".
  constexpr std::uint64_t kTradesPerOrder = 16;
  k.trading.orders = config.node.capacity.orders;
  k.trading.trades = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      std::uint64_t{config.node.capacity.orders} * kTradesPerOrder, UINT32_MAX / 2));
  static_cast<void>(core::FixedString<8>::from(config.node.id, k.trading.node_tag));
  if (!config.node.id.empty()) {
    static_cast<void>(model::TraderId::from(upper(config.node.id) + "-001", k.trading.trader_id));
  }
  if (!config.venues.empty()) {
    static_cast<void>(
        model::AccountId::from(upper(config.venues.front().id) + "-001", k.trading.account_id));
    if (config.venues.front().leverage > 0) {
      k.trading.margin = portfolio::LeveragedMargin{config.venues.front().leverage};
    }
  }
  const RiskSection& r = config.risk;
  risk::RiskConfig& rc = k.trading.risk;
  rc.initial_state = r.initial_state;
  rc.max_order_notional = r.max_order_notional;
  rc.max_position_notional = r.max_position_notional;
  rc.daily_loss_limit = r.daily_loss_limit;
  rc.daily_loss_halt = r.daily_loss_halt;
  rc.max_drawdown = r.max_drawdown;
  rc.price_band_bps = r.price_band_bps;
  rc.max_open_orders = r.max_open_orders;
  rc.orders_per_10s = r.orders_per_10s;
  rc.orders_per_minute = r.orders_per_minute;
  rc.margin_ratio_bps = r.margin_ratio_bps;
  rc.check_margin = r.check_margin;
  return k;
}

void name_strategies(const NodeConfig& config, strategy::KernelServices& kernel) {
  core::FixedVector<model::StrategyId>& ids = kernel.trading.strategy_ids;
  for (std::size_t i = 0; i < config.strategies.size() && i < ids.size(); ++i) {
    const StrategyConfig& entry = config.strategies[i];
    model::StrategyId id;
    if (!entry.id.empty() && core::ok(model::StrategyId::from(entry.id, id))) {
      ids[i] = id;
    }
    // A strategy that lists its instruments may trade only those (InstrumentWhitelistRule).
    if (!entry.instruments.empty()) {
      const auto s = static_cast<std::uint16_t>(i);
      kernel.trading.risk.restrict(s);
      for (const model::InstrumentId& instrument : entry.instruments) {
        model::InstrumentSlot slot;
        if (core::ok(kernel.instruments.intern(instrument, slot))) {
          kernel.trading.risk.allow(s, slot.value);
        }
      }
    }
  }
}

strategy::ErrorPolicy error_policy(const NodeConfig& config) {
  switch (config.risk.on_strategy_error) {
  case OnStrategyError::HaltStrategy:
    return strategy::ErrorPolicy::HaltStrategy;
  case OnStrategyError::HaltNode:
    return strategy::ErrorPolicy::HaltNode;
  case OnStrategyError::Ignore:
    return strategy::ErrorPolicy::Ignore;
  }
  return strategy::ErrorPolicy::HaltStrategy;
}

namespace {

// The day directories of one instrument's definitions, oldest first.
std::vector<std::string> definition_days(const NodeConfig& config,
                                         const model::InstrumentId& instrument) {
  const std::filesystem::path root{
      catalog_directory(config.data.catalog, instrument, "instrument", "")};
  std::vector<std::string> days;
  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(root, ec)) {
    if (item.is_directory()) {
      days.push_back(item.path().filename().string());
    }
  }
  std::sort(days.begin(), days.end());
  return days;
}

// The last definition of `instrument` in the day directory `day`.
Status last_definition(const std::string& directory, const model::InstrumentId& instrument,
                       std::optional<model::Event>& out) {
  LogSource source;
  Status s = source.open({directory});
  if (!core::ok(s)) {
    return s;
  }
  core::EventKey key;
  model::Event event;
  while (core::ok(s = source.next(key, event))) {
    const bool definition = std::holds_alternative<model::CurrencyPair>(event) ||
                            std::holds_alternative<model::CryptoPerpetual>(event) ||
                            std::holds_alternative<model::CryptoFuture>(event);
    if (definition && std::visit(
                          [&instrument](const auto& e) {
                            if constexpr (requires { e.common; }) {
                              return e.common.id == instrument;
                            } else {
                              return false;
                            }
                          },
                          event)) {
      out = event;
    }
  }
  return s == Status::EndOfStream ? Status::Ok : s;
}

} // namespace

namespace {

Status load_definitions(const NodeConfig& config, Preamble& out, std::string& error) {
  std::vector<model::InstrumentId> seen;
  for (const DataStream& group : config.data.streams) {
    for (const model::InstrumentId& instrument : group.instruments) {
      if (std::find(seen.begin(), seen.end(), instrument) != seen.end()) {
        continue;
      }
      seen.push_back(instrument);
      const std::vector<std::string> days = definition_days(config, instrument);
      if (days.empty()) {
        continue; // no definitions: orders for it are denied (INSTRUMENT_UNKNOWN)
      }
      std::string chosen = days.front();
      for (const std::string& day : days) {
        core::UnixNanos start;
        const bool dated =
            day.size() == 10 && core::ok(core::parse_rfc3339(day + "T00:00:00Z", start));
        if (config.data.range && dated && config.data.range->start < start) {
          break;
        }
        chosen = day;
      }
      std::optional<model::Event> definition;
      const std::string directory =
          catalog_directory(config.data.catalog, instrument, "instrument", chosen);
      const Status s = last_definition(directory, instrument, definition);
      if (!core::ok(s)) {
        error = "cannot read instrument definitions in " + directory + ": " +
                std::string{core::to_string(s)};
        return s;
      }
      if (definition) {
        out.events.push_back(*definition);
      }
    }
  }
  return Status::Ok;
}

Status load_account(const NodeConfig& config, const SimSection& sim, Preamble& out,
                    std::string& error) {
  for (const model::Money& total : sim.balances) {
    model::Money zero;
    static_cast<void>(model::Money::from_raw(0, total.currency(), zero));
    model::AccountBalance balance;
    const Status s = model::AccountBalance::create(total, zero, total, balance);
    if (!core::ok(s)) {
      error = "venues.sim.balances: invalid balance";
      return s;
    }
    out.balances.push_back(balance);
  }
  model::AccountState account;
  account.account_id = kernel_config(config).trading.account_id;
  account.account_type = model::AccountType::Margin;
  account.balances = out.balances;
  account.is_reported = true;
  // A valid, reproducible UUID4 (a zero id does not decode).
  constexpr std::uint64_t kAccountSalt = 0xA54FF53A5F1D36F1ULL;
  account.event_id = model::Uuid4::derive(core::CounterRng{config.node.seed ^ kAccountSalt}, 0, 0);
  const core::UnixNanos ts = config.data.range ? config.data.range->start : core::UnixNanos{};
  account.ts_event = ts;
  account.ts_init = ts;
  out.events.push_back(model::Event{account});
  return Status::Ok;
}

} // namespace

Status load_preamble(const NodeConfig& config, Preamble& out, std::string& error) {
  out.events.clear();
  out.balances.clear();
  const Status s = load_definitions(config, out, error);
  if (!core::ok(s) || config.venues.empty()) {
    return s;
  }
  const std::optional<SimSection>& sim = config.venues.front().sim;
  if (!sim || sim->balances.empty()) {
    return Status::Ok;
  }
  return load_account(config, *sim, out, error);
}

Status venue_loop_config(const NodeConfig& config, const strategy::KernelConfig& kernel,
                         backtest::VenueLoopConfig& out, std::string& error) {
  const std::optional<SimSection>& section =
      config.venues.empty() ? std::optional<SimSection>{} : config.venues.front().sim;
  if (!section) {
    error = "no [venues.sim] section";
    return Status::InvalidArgument;
  }
  const SimSection& sim = *section;
  out = backtest::VenueLoopConfig{};
  backtest::SimConfig& c = out.sim;
  c.fill_model = sim.fill_model == FillModel::QueuePosition ? backtest::FillModel::QueuePosition
                                                            : backtest::FillModel::TopOfBook;
  switch (sim.stp) {
  case SelfTradePrevention::None:
    c.stp = backtest::StpMode::None;
    break;
  case SelfTradePrevention::ExpireTaker:
    c.stp = backtest::StpMode::ExpireTaker;
    break;
  case SelfTradePrevention::ExpireMaker:
    c.stp = backtest::StpMode::ExpireMaker;
    break;
  case SelfTradePrevention::ExpireBoth:
    c.stp = backtest::StpMode::ExpireBoth;
    break;
  }
  const std::string schedule = sim.fee_schedule.empty() ? "binance_usdm_vip0" : sim.fee_schedule;
  if (!core::ok(cost::MakerTakerFees::schedule(schedule, c.fees))) {
    error = "venues.sim.fee.schedule: unknown schedule \"" + schedule +
            "\" (binance_usdm_vip0, binance_usdm_vip0_bnb, binance_spot_vip0, "
            "binance_spot_vip0_bnb, zero)";
    return Status::InvalidArgument;
  }
  c.instruments = kernel.instruments;
  c.strategies = kernel.strategies;
  c.orders = kernel.trading.orders;
  c.margin = kernel.trading.margin;
  c.account_id = kernel.trading.account_id;
  c.trader_id = kernel.trading.trader_id;
  c.seed = config.node.seed;
  out.feed_ns = sim.latency.feed_ns;
  out.out_ns = sim.latency.out_ns;
  out.in_ns = sim.latency.in_ns;
  out.jitter_ns = sim.latency.jitter_ns;
  out.seed = config.node.seed;
  if (config.data.range) {
    out.start = config.data.range->start;
    out.end = config.data.range->end;
  }
  return Status::Ok;
}

Status open_sources(const NodeConfig& config, std::vector<LogSource>& sources, std::string& error) {
  std::vector<CatalogStream> streams;
  Status s = resolve_catalog(config.data, streams, error);
  if (!core::ok(s)) {
    return s;
  }
  sources.clear();
  sources.reserve(streams.size());
  for (CatalogStream& stream : streams) {
    sources.emplace_back();
    const std::string first = stream.days.front();
    s = sources.back().open(std::move(stream.days));
    if (!core::ok(s)) {
      error = "cannot open " + first + ": " + std::string{core::to_string(s)};
      return s;
    }
  }
  return Status::Ok;
}

namespace detail {

std::string input_line(const wire::RecordHeader& header, const model::Event& event) {
  return record_text(header, event);
}

std::string output_line(const core::EventKey& key, const model::Output& output) {
  wire::RecordHeader header;
  header.seq = key.seq;
  header.ts = key.ts;
  header.source_id = key.source_id;
  header.kind = static_cast<std::uint16_t>(wire::kind_of(output));
  return record_text(header, output);
}

bool same_record(const core::EventKey& key, const model::Output& output,
                 std::span<const std::byte> recorded) {
  std::array<std::byte, wire::kRecordHeaderSize + 256> buffer{};
  std::size_t written = 0;
  if (!core::ok(wire::encode_output_record(key, output, buffer, written))) {
    return false;
  }
  return written == recorded.size() && std::equal(recorded.begin(), recorded.end(), buffer.begin());
}

std::string failure_text(const strategy::StrategyFailure& failure) {
  return "StrategyError strategy_index=" + std::to_string(failure.strategy) +
         " kind=" + std::string{model::to_string(failure.kind)} +
         " message_hash=" + std::to_string(failure.message_hash);
}

} // namespace detail

} // namespace jarvis::node
