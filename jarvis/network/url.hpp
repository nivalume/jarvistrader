#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"

// Endpoints of the network layer (docs/architecture.md section 13.2).

namespace jarvis::network {

struct Endpoint {
  std::string scheme; // ws, wss, http, https
  std::string host;
  std::uint16_t port = 0;
  std::string target = "/"; // path and query
  [[nodiscard]] bool tls() const noexcept { return scheme == "wss" || scheme == "https"; }
};

// "wss://fstream.binance.com/stream?streams=a/b" -> Endpoint; the port defaults by scheme.
[[nodiscard]] core::Status parse_url(std::string_view url, Endpoint& out);

} // namespace jarvis::network
