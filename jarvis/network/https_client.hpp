#pragma once

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"
#include "jarvis/network/http.hpp"
#include "jarvis/network/io.hpp"

// A blocking HTTP/1.1 client over one keep-alive TLS connection (docs/architecture.md section
// 13.2), for REST calls made at startup and by the order-sender thread. Each request completes
// or fails within the timeout; a request on a stale kept-alive connection is retried once on a
// fresh one.

namespace jarvis::network {

struct HttpsConfig {
  std::string base_url; // https://host[:port] (http:// for local tests)
  TlsOptions tls;
  std::chrono::milliseconds timeout{10'000};
};

class HttpsClient {
public:
  explicit HttpsClient(HttpsConfig config);
  ~HttpsClient();
  HttpsClient(const HttpsClient&) = delete;
  HttpsClient& operator=(const HttpsClient&) = delete;

  [[nodiscard]] core::Status request(std::string_view method, std::string_view target,
                                     std::span<const HeaderField> headers, std::string_view body,
                                     HttpResponse& out, std::string& error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace jarvis::network
