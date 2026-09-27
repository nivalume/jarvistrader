#include "jarvis/node/trace_export.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/order_replay.hpp"

namespace jarvis::node {

namespace {

using core::Status;
namespace ex = execution;

constexpr std::uint64_t kTlcIntMax = 2'147'483'647ULL;

// One event applied to one order, before quantities are scaled.
struct RawStep {
  std::uint64_t seq = 0;
  bool refused = false;
  std::string action; // Plain, Updated, Fill, Void
  std::string kind;   // Plain: the event kind
  std::string trade;  // Fill, Void: the trade id
  std::uint64_t qty = 0;
  // The implementation's order after the step.
  std::string status;
  std::string prev;
  std::uint64_t quantity = 0;
  std::uint64_t filled = 0;
};

struct OrderTrace {
  std::string id;
  std::uint64_t quantity = 0;
  std::vector<RawStep> steps;
};

bool is_order_command(const model::Output& o) {
  return std::holds_alternative<model::SubmitOrder>(o) ||
         std::holds_alternative<model::OrderDenied>(o) ||
         std::holds_alternative<model::ModifyOrder>(o) ||
         std::holds_alternative<model::CancelOrder>(o);
}

class Collector {
public:
  Collector(std::size_t orders, std::size_t fills) : replay_{orders, fills} {}

  void on_output(std::uint64_t seq, const model::Output& output) {
    const ReplayOutcome r = replay_.on_output(output);
    if (!r.known) {
      skipped_ += is_order_command(output) ? 1U : 0U;
      return;
    }
    RawStep step;
    step.action = "Plain";
    step.kind = std::string{ex::to_string(r.kind)};
    record(seq, r, std::move(step));
  }

  void on_event(std::uint64_t seq, const model::OrderEvent& event) {
    const ReplayOutcome r = replay_.on_event(event);
    if (!r.known) {
      ++skipped_;
      return;
    }
    record(seq, r, describe(event));
  }

  [[nodiscard]] std::vector<OrderTrace>& orders() { return orders_; }
  [[nodiscard]] std::size_t skipped() const { return skipped_; }

private:
  void record(std::uint64_t seq, const ReplayOutcome& r, RawStep step) {
    const ex::OrderRecord& o = replay_.oms().at(r.index);
    const std::string key{o.client_order_id.view()};
    auto found = index_.find(key);
    if (found == index_.end()) {
      found = index_.emplace(key, orders_.size()).first;
      orders_.push_back(OrderTrace{key, o.state.quantity().raw(), {}});
    }
    step.seq = seq;
    step.refused = !r.applied;
    step.status = std::string{model::to_string(o.state.status())};
    const std::optional<model::OrderStatus> previous = o.state.previous();
    step.prev = previous ? std::string{model::to_string(*previous)} : "NONE";
    step.quantity = o.state.quantity().raw();
    step.filled = o.state.filled().raw();
    orders_[found->second].steps.push_back(std::move(step));
  }

  static RawStep describe(const model::OrderEvent& event) {
    RawStep step;
    if (const auto* updated = std::get_if<model::OrderUpdated>(&event)) {
      step.action = "Updated";
      step.qty = updated->quantity.raw();
    } else if (const auto* filled = std::get_if<model::OrderFilled>(&event)) {
      step.action = "Fill";
      step.trade = std::string{filled->trade_id.view()};
      step.qty = filled->last_qty.raw();
    } else if (const auto* voided = std::get_if<model::OrderFillVoided>(&event)) {
      step.action = "Void";
      step.trade = std::string{voided->trade_id.view()};
      step.qty = voided->voided_qty.raw();
    } else {
      ex::OrderEventKind kind = ex::OrderEventKind::Denied;
      step.action = "Plain";
      step.kind = ex::kind_of(event, kind) ? std::string{ex::to_string(kind)} : "INITIALIZED";
    }
    return step;
  }

  OrderReplay replay_;
  std::map<std::string, std::size_t, std::less<>> index_;
  std::vector<OrderTrace> orders_;
  std::size_t skipped_ = 0;
};

std::string tla_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out + "\"";
}

Status collect(const std::string& log_dir, Collector& collector) {
  EventLogReader reader;
  Status s = reader.open(log_dir);
  if (!core::ok(s)) {
    return s;
  }
  model::wire::RecordView record;
  while (core::ok(s = reader.next(record))) {
    const std::uint64_t seq = record.header.seq;
    if (record.header.kind >= model::wire::kFirstOutputKind) {
      model::Output output;
      s = EventLogReader::decode_output(record, output);
      if (!core::ok(s)) {
        return s;
      }
      collector.on_output(seq, output);
      continue;
    }
    model::Event event;
    s = reader.decode(record, event);
    if (!core::ok(s)) {
      return s;
    }
    std::visit(
        [&collector, seq](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (kIsOrderEvent<T>) {
            collector.on_event(seq, model::OrderEvent{e});
          }
        },
        event);
  }
  return s == Status::EndOfStream ? Status::Ok : s;
}

// The unit an order's quantities are expressed in: the gcd of all of them.
std::uint64_t unit_of(const OrderTrace& order) {
  std::uint64_t unit = order.quantity;
  for (const RawStep& step : order.steps) {
    if (step.action != "Plain") {
      unit = std::gcd(unit, step.qty);
    }
    unit = std::gcd(unit, std::gcd(step.quantity, step.filled));
  }
  return unit == 0 ? 1 : unit;
}

std::string action_text(const RawStep& step, std::uint64_t unit) {
  if (step.action == "Plain") {
    return "<<\"Plain\", " + tla_string(step.kind) + ">>";
  }
  const std::string q = std::to_string(step.qty / unit);
  if (step.action == "Updated") {
    return "<<\"Updated\", " + q + ">>";
  }
  return "<<" + tla_string(step.action) + ", " + tla_string(step.trade) + ", " + q + ">>";
}

Status write_order_lifecycle(const std::vector<OrderTrace>& orders, const std::string& log_dir,
                             const std::string& out_dir, TraceExportSummary& summary,
                             std::string& error) {
  std::ostringstream body;
  std::uint64_t max_qty = 1;
  for (std::size_t o = 0; o < orders.size(); ++o) {
    const OrderTrace& order = orders[o];
    const std::uint64_t unit = unit_of(order);
    std::uint64_t largest = order.quantity / unit;
    for (const RawStep& step : order.steps) {
      if (step.action == "Updated") {
        largest = std::max(largest, step.qty / unit);
      }
    }
    if (largest > kTlcIntMax) {
      error = "order " + order.id + ": quantities do not fit TLC's integers";
      return Status::OutOfRange;
    }
    max_qty = std::max(max_qty, largest);
    body << (o == 0 ? "  " : ", ") << "[id |-> " << tla_string(order.id) << ", quantity |-> "
         << order.quantity / unit << ", steps |-> <<";
    for (std::size_t k = 0; k < order.steps.size(); ++k) {
      const RawStep& step = order.steps[k];
      body << (k == 0 ? "\n      " : ",\n      ") << "[seq |-> "
           << tla_string(std::to_string(step.seq)) << ", refused |-> "
           << (step.refused ? "TRUE" : "FALSE") << ", a |-> " << action_text(step, unit)
           << ", status |-> " << tla_string(step.status) << ", prev |-> " << tla_string(step.prev)
           << ", quantity |-> " << step.quantity / unit << ", filled |-> " << step.filled / unit
           << "]";
      ++summary.steps;
      summary.refused += step.refused ? 1U : 0U;
    }
    body << ">>]\n";
  }
  summary.orders = orders.size();

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  std::ofstream tla{out_dir + "/OrderLifecycleTrace.tla"};
  tla << "------------------------- MODULE OrderLifecycleTrace -------------------------\n"
         "(* Generated by `jarvis trace-export --spec OrderLifecycle`; do not edit.        *)\n"
         "(* Every order of one event log, its events in log order as OrderLifecycle       *)\n"
         "(* actions (jarvis/node/trace_export.hpp). A refused step is an event the        *)\n"
         "(* implementation refused, which the spec must not enable. Each step also holds  *)\n"
         "(* the implementation's order after it, which the spec's must equal. TLC         *)\n"
         "(* deadlocks at the first step that fails; tools/tla/check_trace.py names it.    *)\n"
      << "(* log: " << std::filesystem::path{log_dir}.filename().string() << " *)\n"
      << "EXTENDS OrderLifecycle, Sequences, TLC\n\n"
         "VARIABLES o, k\n\n"
         "Orders == <<\n"
      << body.str()
      << ">>\n\n"
         "Do(a) == CASE a[1] = \"Plain\" -> Plain(a[2])\n"
         "         []   a[1] = \"Updated\" -> Updated(a[2])\n"
         "         []   a[1] = \"Fill\" -> Fill(a[2], a[3])\n"
         "         []   a[1] = \"Void\" -> Void(a[2], a[3])\n\n"
         "Fresh(q) == /\\ status' = \"INITIALIZED\" /\\ prev' = \"NONE\" /\\ quantity' = q\n"
         "            /\\ filled' = 0 /\\ fills' = [t \\in {} |-> 0]\n\n"
         "TraceInit == /\\ o = 1 /\\ k = 0\n"
         "             /\\ status = \"INITIALIZED\" /\\ prev = \"NONE\"\n"
         "             /\\ quantity = Orders[1].quantity /\\ filled = 0 /\\ fills = [t \\in {} |-> "
         "0]\n\n"
         "Replay == /\\ k < Len(Orders[o].steps)\n"
         "          /\\ LET s == Orders[o].steps[k + 1] IN\n"
         "               /\\ IF s.refused THEN ~ENABLED Do(s.a) /\\ UNCHANGED vars ELSE Do(s.a)\n"
         "               /\\ status' = s.status /\\ prev' = s.prev\n"
         "               /\\ quantity' = s.quantity /\\ filled' = s.filled\n"
         "          /\\ k' = k + 1\n"
         "          /\\ UNCHANGED o\n\n"
         "NextOrder == /\\ k = Len(Orders[o].steps) /\\ o < Len(Orders)\n"
         "             /\\ o' = o + 1 /\\ k' = 0 /\\ Fresh(Orders[o + 1].quantity)\n\n"
         "Done == /\\ k = Len(Orders[o].steps) /\\ o = Len(Orders) /\\ UNCHANGED <<vars, o, k>>\n\n"
         "TraceNext == Replay \\/ NextOrder \\/ Done\n\n"
         "=============================================================================\n";
  std::ofstream cfg{out_dir + "/OrderLifecycleTrace.cfg"};
  cfg << "CONSTANTS\n  MaxQty = " << max_qty
      << "\n  TradeIds = {}\n"
         "INIT TraceInit\n"
         "NEXT TraceNext\n"
         "INVARIANTS TypeOK FilledWithinQuantity FilledIsSumOfFills FullMeansFilled "
         "PendingHasPrevious\n"
         "CHECK_DEADLOCK TRUE\n";
  tla.close();
  cfg.close();
  if (!tla || !cfg) {
    error = "cannot write the trace to " + out_dir;
    return Status::IoError;
  }
  return Status::Ok;
}

} // namespace

Status export_trace(const std::string& log_dir, const std::string& spec, const std::string& out_dir,
                    TraceExportSummary& summary, std::string& error) {
  summary = TraceExportSummary{};
  if (spec != "OrderLifecycle") {
    error = "no trace export for spec " + spec + " (supported: OrderLifecycle)";
    return Status::InvalidArgument;
  }
  std::size_t orders = 0;
  std::size_t fills = 0;
  Status s = OrderReplay::count(log_dir, orders, fills);
  if (!core::ok(s)) {
    error = "cannot read the event log in " + log_dir;
    return s;
  }
  Collector collector{orders, fills};
  s = collect(log_dir, collector);
  if (!core::ok(s)) {
    error = "cannot decode the event log in " + log_dir;
    return s;
  }
  summary.skipped = collector.skipped();
  if (collector.orders().empty()) {
    error = "the log has no orders";
    return Status::NotFound;
  }
  return write_order_lifecycle(collector.orders(), log_dir, out_dir, summary, error);
}

} // namespace jarvis::node
