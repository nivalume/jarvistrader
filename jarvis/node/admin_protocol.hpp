#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/node/config.hpp"

// The admin socket's protocol (docs/architecture.md section 19.3): a Unix domain socket, one
// command per connection, one line each way.
//
//   halt | reduce | resume | cancel_all | shutdown   ->  "ok", or "error <why>"
//   snapshot                                         ->  "ok": an EngineState snapshot at the
//                                                        next batch end
//   set_param <strategy_id> <key> <value>            ->  "ok": the strategy's on_params_changed
//                                                        gets it (true/false and integers are
//                                                        typed, anything else is text; the value
//                                                        is the rest of the line)
//   status                                           ->  one line of JSON: the node state, the
//                                                        TradingState, the last seq, ready, alive
//
// The commands become AdminCommand and ParamUpdate inputs, recorded and replayed like any other;
// status reads what the core thread last published and changes nothing.

namespace jarvis::node {

// [admin] socket ("unix://PATH", {node_id} substituted); empty when there is none.
[[nodiscard]] std::string admin_socket_path(const NodeConfig& config);

// The action a command word names; nullopt for "status" and for words that name nothing.
[[nodiscard]] std::optional<model::AdminAction> admin_action(std::string_view word) noexcept;

// One command for the core thread: an AdminCommand action, or a strategy parameter.
struct AdminRequest {
  model::AdminAction action = model::AdminAction::Halt;
  bool is_param = false;
  model::ParamUpdate param; // with is_param; ts_init is set when the core thread takes it
};

// Parses one command line other than "status"; `strategies` are the node's strategy ids by
// index. Empty on success, else why the line is refused.
[[nodiscard]] std::string parse_admin_request(std::string_view line,
                                              std::span<const std::string> strategies,
                                              AdminRequest& out);

// Sends `line` to the socket at `path` and reads the one-line reply (without the newline).
[[nodiscard]] core::Status admin_request(const std::string& path, std::string_view line,
                                         std::string& reply, std::string& error);

} // namespace jarvis::node
