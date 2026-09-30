#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The I/O loop of one IO thread (docs/architecture.md sections 7.1 and 13.2): an asio::io_context
// behind a small interface, so that no header outside jarvis/network includes Asio.

namespace jarvis::network {

class IoContext {
public:
  IoContext();
  ~IoContext();
  IoContext(const IoContext&) = delete;
  IoContext& operator=(const IoContext&) = delete;

  void run(); // until stop() or no work is left
  std::size_t run_for(std::chrono::milliseconds duration);
  std::size_t run_one_for(std::chrono::milliseconds duration); // until one handler has run
  std::size_t poll();                                          // ready handlers only
  void stop();
  void restart();
  void post(std::function<void()> fn);   // thread-safe
  [[nodiscard]] void* native() noexcept; // asio::io_context*

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// TLS peer verification: the system trust store and SSL_CERT_FILE, plus `ca_file` if given.
struct TlsOptions {
  std::string ca_file;
  bool verify_peer = true;
};

// Nanoseconds of the steady clock at which bytes arrived (the live node maps them to ts_init).
[[nodiscard]] inline std::int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace jarvis::network
