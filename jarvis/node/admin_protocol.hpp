#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/node/config.hpp"

// The admin socket's protocol (docs/architecture.md section 19.3): a Unix domain socket, one
// command per connection, one line each way.
//
//   halt | reduce | resume | cancel_all | shutdown   ->  "ok", or "error <why>"
//   status                                           ->  one line of JSON: the node state, the
//                                                        TradingState, the last seq, ready, alive
//
// The commands become AdminCommand inputs, recorded and replayed like any other; status reads
// what the core thread last published and changes nothing.

namespace jarvis::node {

// [admin] socket ("unix://PATH", {node_id} substituted); empty when there is none.
[[nodiscard]] std::string admin_socket_path(const NodeConfig& config);

// The action a command word names; nullopt for "status" and for words that name nothing.
[[nodiscard]] std::optional<model::AdminAction> admin_action(std::string_view word) noexcept;

// Sends `line` to the socket at `path` and reads the one-line reply (without the newline).
[[nodiscard]] core::Status admin_request(const std::string& path, std::string_view line,
                                         std::string& reply, std::string& error);

} // namespace jarvis::node
