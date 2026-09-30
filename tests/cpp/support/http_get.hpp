#pragma once

#include <sys/socket.h>
#include <unistd.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// A blocking HTTP/1.1 GET to 127.0.0.1:`port` for tests of the telemetry endpoints. The body of
// the answer, and its status in `status` (0 when there was no answer).

namespace jarvis::testsupport {

inline std::string http_get(std::uint16_t port, std::string_view path, int& status) {
  status = 0;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) { // NOLINT
    ::close(fd);
    return {};
  }
  const std::string request = "GET " + std::string{path} + " HTTP/1.1\r\nHost: x\r\n\r\n";
  if (::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) {
    ::close(fd);
    return {};
  }
  std::string response;
  std::array<char, 4096> chunk{};
  ssize_t n = 0;
  while ((n = ::recv(fd, chunk.data(), chunk.size(), 0)) > 0) {
    response.append(chunk.data(), static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (response.starts_with("HTTP/1.1 ") && response.size() >= 12) {
    status = std::stoi(response.substr(9, 3));
  }
  const std::size_t body = response.find("\r\n\r\n");
  return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

} // namespace jarvis::testsupport
