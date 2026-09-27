#include "jarvis/node/order_replay.hpp"

#include <variant>

#include "jarvis/execution/execution_engine.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/event_log.hpp"

namespace jarvis::node {

namespace ex = execution;
using core::Status;

ReplayOutcome OrderReplay::on_output(const model::Output& output) {
  if (const auto* submit = std::get_if<model::SubmitOrder>(&output)) {
    return create(submit, submit->client_order_id, ex::OrderEventKind::Submitted);
  }
  if (const auto* denied = std::get_if<model::OrderDenied>(&output)) {
    return create(nullptr, denied->header.client_order_id, ex::OrderEventKind::Denied);
  }
  if (const auto* modify = std::get_if<model::ModifyOrder>(&output)) {
    return internal(modify->client_order_id, ex::OrderEventKind::PendingUpdate);
  }
  if (const auto* cancel = std::get_if<model::CancelOrder>(&output)) {
    return internal(cancel->client_order_id, ex::OrderEventKind::PendingCancel);
  }
  return ReplayOutcome{};
}

ReplayOutcome OrderReplay::on_event(const model::OrderEvent& event) {
  ReplayOutcome out;
  out.index = oms_.find(model::header_of(event).client_order_id);
  if (out.index == ex::kNoIndex) {
    return out;
  }
  out.known = true;
  static_cast<void>(ex::kind_of(event, out.kind));
  std::uint32_t index = ex::kNoIndex;
  out.applied = ex::apply_order_event(oms_, event, index) == ex::EventOutcome::Applied;
  return out;
}

ReplayOutcome OrderReplay::create(const model::SubmitOrder* submit, const model::ClientOrderId& id,
                                  ex::OrderEventKind kind) {
  if (oms_.find(id) != ex::kNoIndex) {
    return internal(id, kind); // a second command for the same id: apply it as it comes
  }
  ex::OrderRecord record;
  record.client_order_id = id;
  if (submit != nullptr) {
    record.strategy = submit->strategy_index;
    record.instrument_id = submit->instrument_id;
    record.side = submit->order_side;
    record.type = submit->order_type;
    record.time_in_force = submit->time_in_force;
    record.post_only = submit->post_only;
    record.reduce_only = submit->reduce_only;
    record.price = submit->price;
    record.state = ex::OrderState{submit->quantity};
    record.ts_init = submit->ts_init;
  } else {
    model::Quantity one;
    static_cast<void>(model::Quantity::from_raw(1'000'000'000ULL, 0, one));
    record.state = ex::OrderState{one};
  }
  std::uint32_t index = ex::kNoIndex;
  static_cast<void>(oms_.create(record, index));
  return internal(id, kind);
}

ReplayOutcome OrderReplay::internal(const model::ClientOrderId& id, ex::OrderEventKind kind) {
  ReplayOutcome out;
  out.kind = kind;
  out.index = oms_.find(id);
  if (out.index == ex::kNoIndex) {
    return out;
  }
  out.known = true;
  out.applied = core::ok(oms_.apply(out.index, kind));
  return out;
}

Status OrderReplay::count(const std::string& directory, std::size_t& orders, std::size_t& fills) {
  orders = 0;
  fills = 0;
  EventLogReader reader;
  Status s = reader.open(directory);
  if (!core::ok(s)) {
    return s;
  }
  model::wire::RecordView record;
  while (core::ok(s = reader.next(record))) {
    const auto kind = record.header.kind;
    if (kind >= model::wire::kFirstOutputKind) {
      ++orders; // an upper bound: every output
    } else if (kind == static_cast<std::uint16_t>(model::wire::RecordKind::OrderFilled)) {
      ++fills;
    }
  }
  return s == Status::EndOfStream ? Status::Ok : s;
}

} // namespace jarvis::node
