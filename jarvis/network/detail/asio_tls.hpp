#pragma once

// Asio and OpenSSL details shared by the network layer's .cpp files; never included by headers
// that other layers see.

#include <cstdlib>
#include <string>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "jarvis/network/io.hpp"

namespace jarvis::network::detail {

inline asio::ssl::context make_tls_context(const TlsOptions& o) {
  asio::ssl::context ctx{asio::ssl::context::tls_client};
  ctx.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                  asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                  asio::ssl::context::no_tlsv1_1);
  if (o.verify_peer) {
    ctx.set_verify_mode(asio::ssl::verify_peer);
    ctx.set_default_verify_paths();
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once per connection setup
    if (const char* file = std::getenv("SSL_CERT_FILE"); file != nullptr && *file != '\0') {
      asio::error_code ec;
      ctx.load_verify_file(file, ec); // a missing file leaves the system store in place
    }
    if (!o.ca_file.empty()) {
      ctx.load_verify_file(o.ca_file);
    }
  } else {
    ctx.set_verify_mode(asio::ssl::verify_none);
  }
  return ctx;
}

using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

// SNI and hostname verification for a client stream.
inline void prepare_client(TlsStream& s, const std::string& host, bool verify) {
  // SSL_set_tlsext_host_name, without the macro's C-style cast.
  SSL_ctrl(s.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
           static_cast<void*>(
               const_cast<char*>(host.c_str()))); // NOLINT(cppcoreguidelines-pro-type-const-cast)
  if (verify) {
    s.set_verify_callback(asio::ssl::host_name_verification(host));
  }
}

inline asio::io_context& io_of(IoContext& io) {
  return *static_cast<asio::io_context*>(io.native());
}

} // namespace jarvis::network::detail
