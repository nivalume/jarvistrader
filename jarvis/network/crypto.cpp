#include "jarvis/network/crypto.hpp"

#include <array>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdexcept>

namespace jarvis::network {

std::string base64(std::span<const std::byte> data) {
  std::string out(4 * ((data.size() + 2) / 3), '\0');
  const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),        // NOLINT
                                reinterpret_cast<const unsigned char*>(data.data()), // NOLINT
                                static_cast<int>(data.size()));
  out.resize(static_cast<std::size_t>(n));
  return out;
}

std::string base64(std::string_view data) {
  return base64(std::as_bytes(std::span<const char>{data.data(), data.size()}));
}

std::string sha1(std::string_view data) {
  std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
  SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data()); // NOLINT
  return std::string{reinterpret_cast<const char*>(digest.data()), digest.size()};       // NOLINT
}

std::string hmac_sha256(std::string_view key, std::string_view data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int len = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           digest.data(), // NOLINT
           &len) == nullptr) {
    throw std::runtime_error("HMAC-SHA256 failed");
  }
  return std::string{reinterpret_cast<const char*>(digest.data()), len}; // NOLINT
}

std::string hex(std::string_view raw) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (const char c : raw) {
    const auto b = static_cast<unsigned char>(c);
    out += kDigits[b >> 4U];
    out += kDigits[b & 0x0FU];
  }
  return out;
}

} // namespace jarvis::network
