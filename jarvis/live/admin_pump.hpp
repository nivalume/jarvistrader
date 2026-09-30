#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/live/admin_server.hpp"
#include "jarvis/live/live_source.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/strategy/context.hpp"

// The core thread's side of the admin socket (docs/architecture.md sections 5.5 and 19.3): at the
// start of every round of the real-time loop, before the IO threads' rings, the commands become
// AdminCommand inputs, and the kernel's state is published for "status".

namespace jarvis::live {

inline constexpr std::uint16_t kAdminSource = 3;

// The strategy ids set_param names, by strategy index.
[[nodiscard]] inline std::vector<std::string> strategy_names(const strategy::KernelServices& k) {
  std::vector<std::string> out;
  for (const model::StrategyId& id : k.trading.strategy_ids.span()) {
    out.emplace_back(id.view());
  }
  return out;
}

[[nodiscard]] inline core::Status pump_admin(AdminServer* admin,
                                             const strategy::KernelServices* kernel,
                                             LiveSource& source, core::UnixNanos now) {
  if (admin == nullptr) {
    return core::Status::Ok;
  }
  node::AdminRequest request;
  while (admin->commands().try_pop(request)) {
    model::Event event{model::AdminCommand{request.action, now}};
    if (request.is_param) {
      model::ParamUpdate update = request.param;
      update.ts_init = now;
      event = update;
    }
    const core::Status s = source.push_event(event, now, kAdminSource);
    if (!core::ok(s)) {
      return s;
    }
  }
  if (kernel != nullptr) {
    admin->status().publish(kernel->node_state, kernel->trading.risk.trading_state(),
                            kernel->current.seq, network::steady_ns());
  }
  return core::Status::Ok;
}

} // namespace jarvis::live
