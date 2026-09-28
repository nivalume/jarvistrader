#include "jarvis/network/signer.hpp"

#include <array>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdexcept>

#include "jarvis/network/crypto.hpp"

namespace jarvis::network {

std::string HmacSha256Signer::sign(std::string_view payload) const {
  return hex(hmac_sha256(secret_, payload));
}

core::Status Ed25519Signer::from_pem(std::string_view pem, Ed25519Signer& out, std::string& error) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    error = "out of memory";
    return core::Status::CapacityExceeded;
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (key == nullptr) {
    error = "not a PEM private key";
    return core::Status::ParseError;
  }
  if (EVP_PKEY_get_id(key) != EVP_PKEY_ED25519) {
    EVP_PKEY_free(key);
    error = "the key is not an Ed25519 key";
    return core::Status::InvalidArgument;
  }
  out.key_ = std::shared_ptr<void>{key, [](void* k) { EVP_PKEY_free(static_cast<EVP_PKEY*>(k)); }};
  return core::Status::Ok;
}

std::string Ed25519Signer::sign(std::string_view payload) const {
  auto* key = static_cast<EVP_PKEY*>(key_.get());
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  std::array<unsigned char, 64> sig{};
  std::size_t len = sig.size();
  const bool ok = ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
                  EVP_DigestSign(ctx, sig.data(), &len,
                                 reinterpret_cast<const unsigned char*>(payload.data()), // NOLINT
                                 payload.size()) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok) {
    throw std::runtime_error("Ed25519 signing failed");
  }
  return base64(std::as_bytes(std::span<const unsigned char>{sig.data(), len}));
}

core::Status Signer::from_secret(std::string secret, Signer& out, std::string& error) {
  if (secret.find("-----BEGIN") != std::string::npos) {
    Ed25519Signer ed;
    const core::Status s = Ed25519Signer::from_pem(secret, ed, error);
    if (core::ok(s)) {
      out = Signer{std::move(ed)};
    }
    return s;
  }
  while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r')) {
    secret.pop_back();
  }
  if (secret.empty()) {
    error = "empty secret";
    return core::Status::InvalidArgument;
  }
  out = Signer{HmacSha256Signer{std::move(secret)}};
  return core::Status::Ok;
}

std::string Signer::sign(std::string_view payload) const {
  if (const auto* h = std::get_if<HmacSha256Signer>(&impl_)) {
    return h->sign(payload);
  }
  if (const auto* e = std::get_if<Ed25519Signer>(&impl_)) {
    return e->sign(payload);
  }
  throw std::logic_error("signing without a key");
}

} // namespace jarvis::network
