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
#include "jarvis/node/event_text.hpp"
#include "jarvis/node/replay.hpp"

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
  }
  return k;
}

void name_strategies(const NodeConfig& config, strategy::KernelServices& kernel) {
  core::FixedVector<model::StrategyId>& ids = kernel.trading.strategy_ids;
  for (std::size_t i = 0; i < config.strategies.size() && i < ids.size(); ++i) {
    model::StrategyId id;
    if (!config.strategies[i].id.empty() &&
        core::ok(model::StrategyId::from(config.strategies[i].id, id))) {
      ids[i] = id;
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
