#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "jarvis/sys/socket.hpp"

// A blocking HTTP/1.1 GET to 127.0.0.1:`port` for tests of the telemetry endpoints. The body of
// the answer, and its status in `status` (0 when there was no answer).

namespace jarvis::testsupport {

inline std::string http_get(std::uint16_t port, std::string_view path, int& status) {
  status = 0;
  sys::SocketHandle s;
  std::string error;
  if (!core::ok(sys::connect_tcp("127.0.0.1", port, s, error))) {
    return {};
  }
  const std::string request = "GET " + std::string{path} + " HTTP/1.1\r\nHost: x\r\n\r\n";
  if (!sys::send_all(s.get(), request)) {
    return {};
  }
  std::string response;
  std::array<char, 4096> chunk{};
  long n = 0;
  while ((n = sys::receive(s.get(), chunk.data(), chunk.size())) > 0) {
    response.append(chunk.data(), static_cast<std::size_t>(n));
  }
  if (response.starts_with("HTTP/1.1 ") && response.size() >= 12) {
    status = std::stoi(response.substr(9, 3));
  }
  const std::size_t body = response.find("\r\n\r\n");
  return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

} // namespace jarvis::testsupport
