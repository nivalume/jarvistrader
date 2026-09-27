#include "jarvis/network/https_client.hpp"

#include <memory>
#include <vector>

#include "jarvis/network/detail/asio_tls.hpp"
#include "jarvis/network/url.hpp"

namespace jarvis::network {

struct HttpsClient::Impl {
  explicit Impl(HttpsConfig c) : config{std::move(c)}, resolver{io} {}

  HttpsConfig config;
  Endpoint endpoint;
  asio::io_context io{1};
  std::unique_ptr<asio::ssl::context> ssl;
  asio::ip::tcp::resolver resolver;
  std::unique_ptr<detail::TlsStream> stream; // one per connection; no handler outlives wait()
  bool connected = false;
  std::vector<char> buffer = std::vector<char>(std::size_t{64} << 10U);
  HttpResponseParser parser;

  // Runs the loop until `done` or the deadline; on timeout cancels the connection.
  bool wait(const bool& done, std::chrono::steady_clock::time_point deadline) {
    io.restart();
    while (!done) {
      if (io.run_one_until(deadline) == 0) {
        break;
      }
    }
    if (done) {
      return true;
    }
    disconnect();
    io.restart();
    io.run(); // let the canceled handlers finish
    return false;
  }

  void disconnect() {
    connected = false;
    if (stream) {
      asio::error_code ignored;
      stream->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      stream->lowest_layer().close(ignored);
    }
    resolver.cancel();
  }

  template <typename Buffer, typename Handler> void read_some(const Buffer& b, Handler&& h) {
    if (endpoint.tls()) {
      stream->async_read_some(b, std::forward<Handler>(h));
    } else {
      stream->next_layer().async_read_some(b, std::forward<Handler>(h));
    }
  }
  template <typename Buffer, typename Handler> void write_all(const Buffer& b, Handler&& h) {
    if (endpoint.tls()) {
      asio::async_write(*stream, b, std::forward<Handler>(h));
    } else {
      asio::async_write(stream->next_layer(), b, std::forward<Handler>(h));
    }
  }

  core::Status connect(std::chrono::steady_clock::time_point deadline, std::string& error) {
    if (endpoint.host.empty()) {
      if (!core::ok(parse_url(config.base_url, endpoint)) ||
          (endpoint.scheme != "https" && endpoint.scheme != "http")) {
        error = "not an http:// or https:// URL: " + config.base_url;
        return core::Status::InvalidArgument;
      }
      try {
        ssl = std::make_unique<asio::ssl::context>(detail::make_tls_context(config.tls));
      } catch (const std::exception& e) {
        error = std::string{"TLS setup: "} + e.what();
        return core::Status::InvalidArgument;
      }
    }
    stream = std::make_unique<detail::TlsStream>(io, *ssl);
    bool done = false;
    asio::error_code result;
    resolver.async_resolve(
        endpoint.host, std::to_string(endpoint.port),
        [&](const asio::error_code& ec, const asio::ip::tcp::resolver::results_type& results) {
          if (ec) {
            result = ec;
            done = true;
            return;
          }
          asio::async_connect(stream->lowest_layer(), results,
                              [&](const asio::error_code& e, const asio::ip::tcp::endpoint&) {
                                if (e || !endpoint.tls()) {
                                  result = e;
                                  done = true;
                                  return;
                                }
                                detail::prepare_client(*stream, endpoint.host,
                                                       config.tls.verify_peer);
                                stream->async_handshake(asio::ssl::stream_base::client,
                                                        [&](const asio::error_code& h) {
                                                          result = h;
                                                          done = true;
                                                        });
                              });
        });
    if (!wait(done, deadline)) {
      error = "connecting to " + endpoint.host + " timed out";
      return core::Status::IoError;
    }
    if (result) {
      disconnect();
      error = "connecting to " + endpoint.host + ": " + result.message();
      return core::Status::IoError;
    }
    asio::error_code ignored;
    stream->lowest_layer().set_option(asio::ip::tcp::no_delay{true}, ignored);
    connected = true;
    return core::Status::Ok;
  }

  core::Status exchange(const std::string& request, HttpResponse& out,
                        std::chrono::steady_clock::time_point deadline, std::string& error) {
    bool done = false;
    asio::error_code result;
    write_all(asio::buffer(request), [&](const asio::error_code& ec, std::size_t) {
      result = ec;
      done = true;
    });
    if (!wait(done, deadline) || result) {
      error = result ? "write: " + result.message() : std::string{"write timed out"};
      disconnect();
      return core::Status::IoError;
    }
    parser.reset();
    for (;;) {
      done = false;
      std::size_t n = 0;
      read_some(asio::buffer(buffer), [&](const asio::error_code& ec, std::size_t got) {
        result = ec;
        n = got;
        done = true;
      });
      if (!wait(done, deadline)) {
        error = "the response timed out";
        return core::Status::IoError;
      }
      if (result) {
        disconnect();
        if ((result == asio::error::eof || result == asio::ssl::error::stream_truncated) &&
            core::ok(parser.finish())) {
          out = parser.response();
          return core::Status::Ok;
        }
        error = "read: " + result.message();
        return core::Status::IoError;
      }
      std::size_t consumed = 0;
      const core::Status s = parser.feed(std::string_view{buffer.data(), n}, consumed);
      if (s == core::Status::Truncated) {
        continue;
      }
      if (!core::ok(s)) {
        disconnect();
        error = "malformed HTTP response";
        return s;
      }
      out = parser.response();
      if (!parser.keep_alive()) {
        disconnect();
      }
      return core::Status::Ok;
    }
  }
};

HttpsClient::HttpsClient(HttpsConfig config) : impl_{std::make_unique<Impl>(std::move(config))} {}
HttpsClient::~HttpsClient() = default;

core::Status HttpsClient::request(std::string_view method, std::string_view target,
                                  std::span<const HeaderField> headers, std::string_view body,
                                  HttpResponse& out, std::string& error) {
  Impl& d = *impl_;
  const auto deadline = std::chrono::steady_clock::now() + d.config.timeout;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool reused = d.connected;
    if (!d.connected) {
      const core::Status s = d.connect(deadline, error);
      if (!core::ok(s)) {
        return s;
      }
    }
    const std::string request = http_request(method, d.endpoint.host, target, headers, body);
    const core::Status s = d.exchange(request, out, deadline, error);
    if (core::ok(s) || !reused) {
      return s;
    }
    // A kept-alive connection the server had closed: once more on a fresh one.
  }
  return core::Status::IoError;
}

} // namespace jarvis::network
