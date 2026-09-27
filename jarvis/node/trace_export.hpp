#pragma once

#include <cstddef>
#include <string>

#include "jarvis/core/status.hpp"

// Backward trace validation (docs/architecture.md 18.2): an event log projected on the
// variables of a spec, as a TLA+ module that TLC checks (tools/tla/check_trace.py).
//
// OrderLifecycle: every order's events in log order. The log holds the venue's order events as
// inputs and the kernel's commands as outputs; the events the kernel applied itself are
// recovered from the commands, as Trading does them:
//
//   SubmitOrder   the order is created with its quantity, then SUBMITTED
//   OrderDenied   the order is created (quantity 1: the denial does not carry it), then DENIED
//   ModifyOrder   PENDING_UPDATE
//   CancelOrder   PENDING_CANCEL
//   order event   the event itself (from the venue)
//
// A fresh OMS applies them with the kernel's own code (execution::apply_order_event); what it
// refuses (an invalid transition, a duplicate fill) becomes a refused step, which the spec must
// refuse too. Events of orders the log never created are skipped. Quantities are expressed in
// units of the greatest common divisor of the order's quantities, so they fit TLC's integers.

namespace jarvis::node {

struct TraceExportSummary {
  std::size_t orders = 0;
  std::size_t steps = 0;
  std::size_t refused = 0;
  std::size_t skipped = 0; // events of orders the log never created
};

// Writes <out_dir>/<spec>Trace.tla and <spec>Trace.cfg. Only "OrderLifecycle" is supported.
[[nodiscard]] core::Status export_trace(const std::string& log_dir, const std::string& spec,
                                        const std::string& out_dir, TraceExportSummary& summary,
                                        std::string& error);

} // namespace jarvis::node
