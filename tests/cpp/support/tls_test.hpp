#pragma once

// TLS helpers for tests that talk to loopback servers: a temporary directory, a self-signed
// certificate made at test time, blocking HTTP head reading, and a scripted HTTPS server.

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <doctest/doctest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace jarvis::testsupport {

class TempDir {
public:
  TempDir() {
    path_ = std::filesystem::temp_directory_path() /
            ("jarvis-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] std::string file(std::string_view name) const { return (path_ / name).string(); }

private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

// A self-signed certificate for localhost and 127.0.0.1 (its own CA).
inline void make_certificate(const std::string& cert_path, const std::string& key_path) {
  EVP_PKEY* key = EVP_EC_gen("P-256");
  REQUIRE(key != nullptr);
  X509* x = X509_new();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_getm_notBefore(x), -60);
  X509_gmtime_adj(X509_getm_notAfter(x), 3600);
  X509_set_pubkey(x, key);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
                             0); // NOLINT
  X509_set_issuer_name(x, name);
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
  for (const auto& [nid, value] : {std::pair{NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"},
                                   std::pair{NID_basic_constraints, "critical,CA:TRUE"}}) {
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    REQUIRE(ext != nullptr);
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
  }
  REQUIRE(X509_sign(x, key, EVP_sha256()) > 0);
  BIO* cert = BIO_new_file(cert_path.c_str(), "w");
  PEM_write_bio_X509(cert, x);
  BIO_free(cert);
  BIO* priv = BIO_new_file(key_path.c_str(), "w");
  PEM_write_bio_PrivateKey(priv, key, nullptr, nullptr, 0, nullptr, nullptr);
  BIO_free(priv);
  X509_free(x);
  EVP_PKEY_free(key);
}

template <typename Stream> std::string read_head(Stream& s) {
  std::string head;
  char c = 0;
  while (head.find("\r\n\r\n") == std::string::npos) {
    asio::read(s, asio::buffer(&c, 1));
    head += c;
  }
  return head;
}

inline std::string header_value(const std::string& head, std::string_view name) {
  const std::size_t at = head.find(std::string{name} + ": ");
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + name.size() + 2;
  return head.substr(start, head.find("\r\n", start) - start);
}

// Serves raw HTTP responses in order over TLS, one per request, and records each request (head
// and body). A response with "Connection: close" ends its connection; the next request comes on
// a new one. Stops after the last response.
class ScriptedHttpsServer {
public:
  explicit ScriptedHttpsServer(std::vector<std::string> responses)
      : ScriptedHttpsServer{std::move(responses), {}, {}} {}
  // With another server's certificate and key, so one CA file trusts both.
  ScriptedHttpsServer(std::vector<std::string> responses, std::string cert, std::string key)
      : ssl_{asio::ssl::context::tls_server}, responses_{std::move(responses)} {
    if (cert.empty()) {
      cert = dir_.file("cert.pem");
      key = dir_.file("key.pem");
      make_certificate(cert, key);
    }
    cert_ = cert;
    ssl_.use_certificate_chain_file(cert);
    ssl_.use_private_key_file(key, asio::ssl::context::pem);
    acceptor_.open(asio::ip::tcp::v4());
    acceptor_.bind({asio::ip::make_address("127.0.0.1"), 0});
    acceptor_.listen();
    thread_ = std::thread{[this] { serve(); }};
  }
  ~ScriptedHttpsServer() { join(); }
  ScriptedHttpsServer(const ScriptedHttpsServer&) = delete;
  ScriptedHttpsServer& operator=(const ScriptedHttpsServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
  [[nodiscard]] std::string ca_file() const { return cert_; }
  [[nodiscard]] std::string url() const { return "https://localhost:" + std::to_string(port()); }
  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  [[nodiscard]] std::vector<std::string> requests() {
    const std::lock_guard lock{mutex_};
    return requests_;
  }
  [[nodiscard]] const std::string& error() const { return error_; }

private:
  void serve() {
    try {
      std::size_t next = 0;
      while (next < responses_.size()) {
        asio::ssl::stream<asio::ip::tcp::socket> s{io_, ssl_};
        acceptor_.accept(s.lowest_layer());
        s.handshake(asio::ssl::stream_base::server);
        while (next < responses_.size()) {
          std::string request = read_head(s);
          const std::string length = header_value(request, "Content-Length");
          if (!length.empty() && std::stoul(length) > 0) {
            std::string body(std::stoul(length), '\0');
            asio::read(s, asio::buffer(body));
            request += body;
          }
          {
            const std::lock_guard lock{mutex_};
            requests_.push_back(request);
          }
          const std::string& response = responses_[next++];
          asio::write(s, asio::buffer(response));
          if (response.find("Connection: close") != std::string::npos) {
            break;
          }
        }
        asio::error_code ec;
        s.lowest_layer().close(ec);
      }
    } catch (const std::exception& e) {
      error_ = e.what();
    }
  }

  TempDir dir_;
  std::string cert_;
  asio::io_context io_;
  asio::ssl::context ssl_;
  asio::ip::tcp::acceptor acceptor_{io_};
  std::vector<std::string> responses_;
  std::vector<std::string> requests_;
  std::mutex mutex_;
  std::string error_;
  std::thread thread_;
};

// Answers each HTTPS request with `handler(request)` (the request line, headers and body), one
// request per connection: every response closes it, so requests are served strictly in turn.
// Serves until destroyed.
class HandlerHttpsServer {
public:
  using Handler = std::function<std::string(const std::string& request)>;

  HandlerHttpsServer(Handler handler, std::string cert, const std::string& key)
      : ssl_{asio::ssl::context::tls_server}, handler_{std::move(handler)}, cert_{std::move(cert)} {
    ssl_.use_certificate_chain_file(cert_);
    ssl_.use_private_key_file(key, asio::ssl::context::pem);
    acceptor_.open(asio::ip::tcp::v4());
    acceptor_.bind({asio::ip::make_address("127.0.0.1"), 0});
    acceptor_.listen();
    thread_ = std::thread{[this] { serve(); }};
  }
  ~HandlerHttpsServer() {
    stopping_ = true;
    try { // wake the blocking accept
      asio::io_context io;
      asio::ip::tcp::socket s{io};
      s.connect({asio::ip::make_address("127.0.0.1"), port()});
    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch): already stopped
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  HandlerHttpsServer(const HandlerHttpsServer&) = delete;
  HandlerHttpsServer& operator=(const HandlerHttpsServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
  [[nodiscard]] std::string url() const { return "https://localhost:" + std::to_string(port()); }
  [[nodiscard]] std::string error() {
    const std::lock_guard lock{mutex_};
    return error_;
  }

private:
  void serve() {
    while (!stopping_) {
      asio::ssl::stream<asio::ip::tcp::socket> s{io_, ssl_};
      asio::error_code ec;
      acceptor_.accept(s.lowest_layer(), ec);
      if (ec || stopping_) {
        break;
      }
      try {
        s.handshake(asio::ssl::stream_base::server);
        std::string request = read_head(s);
        const std::string length = header_value(request, "Content-Length");
        if (!length.empty() && std::stoul(length) > 0) {
          std::string body(std::stoul(length), '\0');
          asio::read(s, asio::buffer(body));
          request += body;
        }
        const std::string response = handler_(request);
        asio::write(s, asio::buffer(response));
      } catch (const std::exception& e) {
        // A client that gave up on a request (shutting down) is not an error of the server.
        const std::lock_guard lock{mutex_};
        if (error_.empty() && !stopping_) {
          error_ = e.what();
        }
      }
      s.lowest_layer().close(ec);
    }
  }

  asio::io_context io_;
  asio::ssl::context ssl_;
  asio::ip::tcp::acceptor acceptor_{io_};
  Handler handler_;
  std::string cert_;
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::string error_;
  std::thread thread_;
};

} // namespace jarvis::testsupport
