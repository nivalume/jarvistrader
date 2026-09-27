#include "jarvis/network/url.hpp"

#include <charconv>
#include <system_error>

namespace jarvis::network {

core::Status parse_url(std::string_view url, Endpoint& out) {
  const std::size_t sep = url.find("://");
  if (sep == std::string_view::npos) {
    return core::Status::ParseError;
  }
  Endpoint e;
  e.scheme = std::string{url.substr(0, sep)};
  std::uint16_t default_port = 0;
  if (e.scheme == "wss" || e.scheme == "https") {
    default_port = 443;
  } else if (e.scheme == "ws" || e.scheme == "http") {
    default_port = 80;
  } else {
    return core::Status::InvalidArgument;
  }
  std::string_view rest = url.substr(sep + 3);
  const std::size_t slash = rest.find_first_of("/?");
  std::string_view authority = rest.substr(0, slash);
  if (slash != std::string_view::npos) {
    e.target = std::string{rest.substr(slash)};
    if (e.target.front() == '?') {
      e.target.insert(e.target.begin(), '/');
    }
  }
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
    const std::string_view digits = authority.substr(colon + 1);
    unsigned port = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (ec != std::errc{} || end != digits.data() + digits.size() || port == 0 || port > 65535) {
      return core::Status::ParseError;
    }
    e.port = static_cast<std::uint16_t>(port);
    authority = authority.substr(0, colon);
  } else {
    e.port = default_port;
  }
  if (authority.empty()) {
    return core::Status::ParseError;
  }
  e.host = std::string{authority};
  out = std::move(e);
  return core::Status::Ok;
}

} // namespace jarvis::network
