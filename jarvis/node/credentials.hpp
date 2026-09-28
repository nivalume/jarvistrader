#pragma once

#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"

// venues[].credentials (docs/architecture.md section 19.1): a reference to the API key, never the
// key itself.
//
//   env:PREFIX   PREFIX_API_KEY, and PREFIX_API_SECRET or PREFIX_PRIVATE_KEY_FILE
//   file:PATH    a TOML file: api_key, and secret or private_key_file (relative to the file)
//
// The secret is an HMAC secret or an Ed25519 private key in PEM (the WebSocket API then logs on
// once instead of signing every request). A file holding a secret (the TOML file, a private key
// file) must be readable by its owner only (mode 0600 or 0400); otherwise resolving fails.
// Errors name what is missing, never a secret.

namespace jarvis::node {

struct ApiCredentials {
  std::string api_key;
  std::string secret; // HMAC secret or PEM text
};

[[nodiscard]] core::Status resolve_credentials(std::string_view reference, ApiCredentials& out,
                                               std::string& error);

} // namespace jarvis::node
