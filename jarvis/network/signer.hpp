#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "jarvis/core/status.hpp"

// Request signing (docs/architecture.md sections 13.2 and 19): HMAC-SHA256 (hex, Binance REST
// with an HMAC key) and Ed25519 (base64, REST with an Ed25519 key and the WS API session.logon).
// Keys are never in configuration files: node/credentials.hpp resolves a credentials reference.

namespace jarvis::network {

class HmacSha256Signer {
public:
  explicit HmacSha256Signer(std::string secret) : secret_{std::move(secret)} {}
  [[nodiscard]] std::string sign(std::string_view payload) const; // lowercase hex
private:
  std::string secret_;
};

class Ed25519Signer {
public:
  // A PKCS#8 PEM private key ("-----BEGIN PRIVATE KEY-----").
  [[nodiscard]] static core::Status from_pem(std::string_view pem, Ed25519Signer& out,
                                             std::string& error);
  [[nodiscard]] std::string sign(std::string_view payload) const; // base64
  [[nodiscard]] bool valid() const noexcept { return key_ != nullptr; }

private:
  std::shared_ptr<void> key_; // EVP_PKEY, freed with EVP_PKEY_free
};

class Signer {
public:
  Signer() = default;
  explicit Signer(HmacSha256Signer s) : impl_{std::move(s)} {}
  explicit Signer(Ed25519Signer s) : impl_{std::move(s)} {}

  // An HMAC secret, or a PEM Ed25519 key when the secret is PEM text.
  [[nodiscard]] static core::Status from_secret(std::string secret, Signer& out,
                                                std::string& error);

  [[nodiscard]] std::string sign(std::string_view payload) const;
  [[nodiscard]] bool ed25519() const noexcept {
    return std::holds_alternative<Ed25519Signer>(impl_);
  }
  [[nodiscard]] bool empty() const noexcept {
    return std::holds_alternative<std::monostate>(impl_);
  }

private:
  std::variant<std::monostate, HmacSha256Signer, Ed25519Signer> impl_;
};

} // namespace jarvis::network
