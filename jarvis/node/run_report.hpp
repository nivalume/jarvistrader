#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/money.hpp"

// The report of a backtest (docs/architecture.md section 12.4): fills, fees and PnL per strategy
// and instrument, the orders' fate, and the account, labelled with the data and the fill model
// the numbers come from. It is computed from the run directory alone (config.toml and the run
// log): orders go through a fresh OMS (node/order_replay.hpp) and applied fills through a
// Portfolio, with the kernel's own code, so the numbers are the kernel's.

namespace jarvis::node {

struct ReportRow {
  std::string strategy;
  std::string instrument;
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  std::uint64_t taker_fills = 0;
  model::Quantity bought;
  model::Quantity sold;
  model::Money notional;   // traded notional in the settlement currency
  model::Money commission; // paid (positive)
  model::Money funding;    // received (positive) or paid
  model::Money realized;   // gross of commission and funding
  model::Money unrealized; // at the valuation price
  model::Money net;        // realized - commission + funding + unrealized
  model::Decimal position; // signed, at the end
  std::optional<model::Price> avg_px_open;
  std::optional<model::Price> valuation;
  std::string valuation_source; // "mark", "last trade", "last quote mid" or "none"
};

struct OrderCounts {
  std::uint64_t submitted = 0;
  std::uint64_t denied = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t canceled = 0;
  std::uint64_t expired = 0;
  std::uint64_t filled = 0; // orders that ended FILLED
  std::uint64_t modifies = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modify_rejected = 0;
  std::uint64_t cancel_rejected = 0;
  std::uint64_t refused = 0; // venue events the OMS refused (duplicates, late events)
};

struct RunReport {
  std::string directory;
  std::string node_id;
  std::string env;
  std::uint64_t seed = 0;
  // What the numbers rest on.
  std::string catalog;
  std::string range; // "start to end", empty for the whole catalog
  std::vector<std::string> streams;
  std::vector<std::string> instruments;
  std::string book;  // the market data the fills were simulated against
  std::string venue; // the simulated venue's models, or "none (data only)"
  std::string fill_model;
  std::string fee_schedule;
  // Results.
  std::vector<model::Money> starting_balances;
  std::vector<model::Money> ending_balances;
  std::vector<ReportRow> rows;
  std::vector<std::pair<std::string, OrderCounts>> orders; // by strategy
  std::uint64_t first_ts = 0;
  std::uint64_t last_ts = 0;
};

[[nodiscard]] core::Status build_run_report(const std::string& directory, RunReport& out,
                                            std::string& error);

// The report as text, one fact per line.
[[nodiscard]] std::string report_text(const RunReport& report);

} // namespace jarvis::node
