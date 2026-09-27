#include "jarvis/node/backtest_node.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/instrument_table.hpp"
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
