#pragma once

#include <cstdint>

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

[[nodiscard]] inline core::Status pump_admin(AdminServer* admin,
                                             const strategy::KernelServices* kernel,
                                             LiveSource& source, core::UnixNanos now) {
  if (admin == nullptr) {
    return core::Status::Ok;
  }
  model::AdminAction action{};
  while (admin->commands().try_pop(action)) {
    const core::Status s =
        source.push_event(model::Event{model::AdminCommand{action, now}}, now, kAdminSource);
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
