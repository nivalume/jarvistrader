#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// Small OpenSSL wrappers the network layer needs (docs/architecture.md section 13.2).

namespace jarvis::network {

[[nodiscard]] std::string base64(std::span<const std::byte> data);
[[nodiscard]] std::string base64(std::string_view data);
[[nodiscard]] std::string sha1(std::string_view data);                              // 20 raw bytes
[[nodiscard]] std::string hmac_sha256(std::string_view key, std::string_view data); // 32 raw bytes
[[nodiscard]] std::string hex(std::string_view raw);

} // namespace jarvis::network
