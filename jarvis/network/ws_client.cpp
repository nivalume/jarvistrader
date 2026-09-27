#include "jarvis/network/ws_client.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <random>

#include "jarvis/network/detail/asio_tls.hpp"
#include "jarvis/network/url.hpp"

namespace jarvis::network {

namespace {

constexpr std::chrono::seconds kCloseTimeout{5};

class MaskSource {
public:
  MaskSource() : state_{std::random_device{}()} {}
  std::array<std::uint8_t, 4> mask() noexcept {
    const std::uint64_t z = next();
    return {static_cast<std::uint8_t>(z), static_cast<std::uint8_t>(z >> 8U),
            static_cast<std::uint8_t>(z >> 16U), static_cast<std::uint8_t>(z >> 24U)};
  }
  std::array<std::uint8_t, 16> nonce() noexcept {
    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < out.size(); i += 8) {
      const std::uint64_t z = next();
      for (std::size_t k = 0; k < 8; ++k) {
        out[i + k] = static_cast<std::uint8_t>(z >> (8U * k));
      }
    }
    return out;
  }

private:
  std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::uint64_t state_;
};

} // namespace

struct WsClient::Impl : std::enable_shared_from_this<Impl> {
  enum class Phase : std::uint8_t { Idle, Connecting, Handshake, Open, Closing, Closed };

  Impl(asio::io_context& context, WsConfig c, WsHandlers h)
      : io{context}, config{std::move(c)}, handlers{std::move(h)}, resolver{context},
        timer{context}, decoder{config.max_message} {
    buffer.resize(config.receive_buffer < 1024 ? 1024 : config.receive_buffer);
  }

  asio::io_context& io;
  WsConfig config;
  WsHandlers handlers;
  Endpoint endpoint;
  std::unique_ptr<asio::ssl::context> ssl;
  asio::ip::tcp::resolver resolver;
  // One stream per connection. Every completion handler holds its stream (so a stream outlives
  // its operations) and the generation it was started in: a completion from an earlier
  // connection, say an aborted read delivered after on_close reconnected, is ignored.
  std::shared_ptr<detail::TlsStream> stream;
  std::uint64_t generation = 0;
  asio::steady_timer timer;
  std::vector<std::byte> buffer;
  std::size_t filled = 0;
  WsDecoder decoder;
  std::vector<WsMessage> messages;
  std::deque<std::vector<std::byte>> outbox;
  std::vector<std::vector<std::byte>> waiting; // queued before the connection opened
  bool writing = false;
  bool close_sent = false;
  bool shutdown_after_writes = false; // the close was answered: end once the outbox drains
  std::string close_reason;
  std::string key;
  std::string request;
  Phase phase = Phase::Idle;
  MaskSource masks;
  std::atomic<bool> open{false};
  std::atomic<std::uint64_t> n_messages{0};
  std::atomic<std::uint64_t> n_bytes{0};
  std::atomic<std::uint64_t> n_pings{0};
  std::atomic<std::uint64_t> n_sent{0};

  template <typename Buffer, typename Handler>
  void read_some(detail::TlsStream& s, const Buffer& b, Handler&& h) {
    if (endpoint.tls()) {
      s.async_read_some(b, std::forward<Handler>(h));
    } else {
      s.next_layer().async_read_some(b, std::forward<Handler>(h));
    }
  }
  template <typename Buffer, typename Handler>
  void write_all(detail::TlsStream& s, const Buffer& b, Handler&& h) {
    if (endpoint.tls()) {
      asio::async_write(s, b, std::forward<Handler>(h));
    } else {
      asio::async_write(s.next_layer(), b, std::forward<Handler>(h));
    }
  }
  [[nodiscard]] bool current(std::uint64_t gen) const noexcept {
    return gen == generation && phase != Phase::Closed;
  }

  void fail(const std::string& reason) {
    if (phase == Phase::Closed) {
      return;
    }
    phase = Phase::Closed;
    open = false;
    timer.cancel();
    resolver.cancel();
    if (const std::shared_ptr<detail::TlsStream> s = stream) {
      asio::error_code ignored;
      s->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      s->lowest_layer().close(ignored);
    }
    if (handlers.on_close) {
      handlers.on_close(reason);
    }
  }

  void connect() {
    if (phase != Phase::Idle && phase != Phase::Closed) {
      return;
    }
    if (!core::ok(parse_url(config.url, endpoint)) ||
        (endpoint.scheme != "ws" && endpoint.scheme != "wss")) {
      phase = Phase::Idle;
      fail("not a ws:// or wss:// URL: " + config.url);
      return;
    }
    try {
      ssl = std::make_unique<asio::ssl::context>(detail::make_tls_context(config.tls));
    } catch (const std::exception& e) {
      fail(std::string{"TLS setup: "} + e.what());
      return;
    }
    stream = std::make_shared<detail::TlsStream>(io, *ssl);
    ++generation;
    filled = 0;
    decoder.reset();
    outbox.clear();
    writing = false;
    close_sent = false;
    shutdown_after_writes = false;
    close_reason.clear();
    phase = Phase::Connecting;
    const std::array<std::uint8_t, 16> nonce = masks.nonce();
    key = ws_key_from(nonce);
    request = ws_upgrade_request(endpoint.host, endpoint.port, endpoint.tls(), endpoint.target, key,
                                 config.headers);
    timer.expires_after(config.handshake_timeout);
    timer.async_wait([self = shared_from_this(), gen = generation](const asio::error_code& ec) {
      if (!ec && self->current(gen) &&
          (self->phase == Phase::Connecting || self->phase == Phase::Handshake)) {
        self->fail("the connection or handshake timed out");
      }
    });
    resolver.async_resolve(
        endpoint.host, std::to_string(endpoint.port),
        [self = shared_from_this(), s = stream, gen = generation](
            const asio::error_code& ec, const asio::ip::tcp::resolver::results_type& results) {
          if (!self->current(gen)) {
            return;
          }
          if (ec) {
            self->fail("resolve " + self->endpoint.host + ": " + ec.message());
            return;
          }
          asio::async_connect(
              s->lowest_layer(), results,
              [self, s, gen](const asio::error_code& e, const asio::ip::tcp::endpoint&) {
                if (self->current(gen)) {
                  self->on_connected(*s, gen, e);
                }
              });
        });
  }

  void on_connected(detail::TlsStream& s, std::uint64_t gen, const asio::error_code& ec) {
    if (ec) {
      fail("connect " + endpoint.host + ": " + ec.message());
      return;
    }
    asio::error_code ignored;
    s.lowest_layer().set_option(asio::ip::tcp::no_delay{true}, ignored);
    if (!endpoint.tls()) {
      send_upgrade();
      return;
    }
    detail::prepare_client(s, endpoint.host, config.tls.verify_peer);
    s.async_handshake(asio::ssl::stream_base::client, [self = shared_from_this(), keep = stream,
                                                       gen](const asio::error_code& e) {
      if (!self->current(gen)) {
        return;
      }
      if (e) {
        self->fail("TLS handshake with " + self->endpoint.host + ": " + e.message());
        return;
      }
      self->send_upgrade();
    });
  }

  void send_upgrade() {
    write_all(*stream, asio::buffer(request),
              [self = shared_from_this(), keep = stream,
               gen = generation](const asio::error_code& ec, std::size_t) {
                if (!self->current(gen)) {
                  return;
                }
                if (ec) {
                  self->fail("sending the upgrade: " + ec.message());
                  return;
                }
                self->phase = Phase::Handshake;
                self->start_read();
              });
  }

  void start_read() {
    if (filled == buffer.size()) {
      if (buffer.size() >= config.max_message + 14) {
        fail("a message larger than max_message");
        return;
      }
      buffer.resize(buffer.size() * 2);
    }
    read_some(*stream, asio::buffer(buffer.data() + filled, buffer.size() - filled),
              [self = shared_from_this(), keep = stream,
               gen = generation](const asio::error_code& ec, std::size_t n) {
                if (self->current(gen)) {
                  self->on_read(ec, n);
                }
              });
  }

  void on_read(const asio::error_code& ec, std::size_t n) {
    if (phase == Phase::Closed) {
      return;
    }
    if (ec) {
      fail(phase == Phase::Closing ? "closed" : "read: " + ec.message());
      return;
    }
    const std::int64_t now = steady_ns();
    filled += n;
    if (phase == Phase::Handshake) {
      std::size_t head = 0;
      std::string error;
      const std::string_view text{reinterpret_cast<const char*>(buffer.data()), filled}; // NOLINT
      const core::Status s = ws_parse_upgrade(text, key, head, error);
      if (s == core::Status::Truncated) {
        start_read();
        return;
      }
      if (!core::ok(s)) {
        fail("upgrade refused: " + error);
        return;
      }
      std::memmove(buffer.data(), buffer.data() + head, filled - head);
      filled -= head;
      phase = Phase::Open;
      open = true;
      timer.cancel();
      for (std::vector<std::byte>& frame : waiting) {
        outbox.push_back(std::move(frame));
      }
      waiting.clear();
      if (handlers.on_open) {
        handlers.on_open();
      }
      if (phase != Phase::Open) {
        return; // on_open closed it
      }
      flush();
    }
    std::size_t consumed = 0;
    const core::Status s =
        decoder.decode(std::span<const std::byte>{buffer.data(), filled}, consumed, messages);
    for (const WsMessage& m : messages) {
      dispatch(m, now);
      if (phase == Phase::Closed) {
        return;
      }
    }
    if (!core::ok(s)) {
      fail("protocol error: " + decoder.error());
      return;
    }
    std::memmove(buffer.data(), buffer.data() + consumed, filled - consumed);
    filled -= consumed;
    start_read();
  }

  void dispatch(const WsMessage& m, std::int64_t now) {
    switch (m.opcode) {
    case WsOpcode::Text:
    case WsOpcode::Binary:
      n_messages.fetch_add(1, std::memory_order_relaxed);
      n_bytes.fetch_add(m.payload.size(), std::memory_order_relaxed);
      if (handlers.on_message) {
        handlers.on_message(m.opcode, m.payload, now);
      }
      break;
    case WsOpcode::Ping:
      n_pings.fetch_add(1, std::memory_order_relaxed);
      queue(WsOpcode::Pong, m.payload);
      break;
    case WsOpcode::Close: {
      std::uint16_t code = 1005;
      std::string reason;
      if (m.payload.size() >= 2) {
        code = static_cast<std::uint16_t>((std::to_integer<unsigned>(m.payload[0]) << 8U) |
                                          std::to_integer<unsigned>(m.payload[1]));
        reason.assign(reinterpret_cast<const char*>(m.payload.data()) + 2, // NOLINT
                      m.payload.size() - 2);
      }
      if (!close_sent) { // the server's close: echo it
        const std::vector<std::byte> echo = ws_close_payload(code == 1005 ? 1000 : code, {});
        queue(WsOpcode::Close, echo);
        close_sent = true;
        close_reason =
            "closed by the server: " + std::to_string(code) + (reason.empty() ? "" : " " + reason);
      } // else the answer to our close: close_reason stays as start_close set it
      phase = Phase::Closing;
      shutdown_after_writes = true;
      if (!writing) {
        fail(close_reason);
      }
      break;
    }
    case WsOpcode::Pong:
    case WsOpcode::Continuation:
      break;
    }
  }

  void queue(WsOpcode op, std::span<const std::byte> payload) {
    std::vector<std::byte> frame;
    frame.reserve(payload.size() + 14);
    ws_encode(op, payload, masks.mask(), frame);
    if (phase != Phase::Open && phase != Phase::Closing) {
      waiting.push_back(std::move(frame));
      return;
    }
    outbox.push_back(std::move(frame));
    flush();
  }

  void flush() {
    if (writing || outbox.empty()) {
      return;
    }
    writing = true;
    write_all(*stream, asio::buffer(outbox.front()),
              [self = shared_from_this(), keep = stream,
               gen = generation](const asio::error_code& ec, std::size_t) {
                if (!self->current(gen)) {
                  return; // connect() resets the write state of a new connection
                }
                self->writing = false;
                if (ec) {
                  self->fail("write: " + ec.message());
                  return;
                }
                self->n_sent.fetch_add(1, std::memory_order_relaxed);
                self->outbox.pop_front();
                if (!self->outbox.empty()) {
                  self->flush();
                } else if (self->shutdown_after_writes) {
                  self->fail(self->close_reason);
                }
              });
  }

  void start_close(std::uint16_t code, const std::string& reason) {
    if (phase != Phase::Open) {
      fail(reason.empty() ? "closed" : reason);
      return;
    }
    const std::vector<std::byte> payload = ws_close_payload(code, reason);
    queue(WsOpcode::Close, payload);
    close_sent = true;
    phase = Phase::Closing;
    close_reason = "closed";
    timer.expires_after(kCloseTimeout);
    timer.async_wait([self = shared_from_this(), gen = generation](const asio::error_code& ec) {
      if (!ec && self->current(gen) && self->phase == Phase::Closing) {
        self->fail("closed (the server did not answer the close)");
      }
    });
  }
};

WsClient::WsClient(IoContext& io, WsConfig config, WsHandlers handlers)
    : impl_{std::make_shared<Impl>(detail::io_of(io), std::move(config), std::move(handlers))} {}

WsClient::~WsClient() {
  impl_->handlers = WsHandlers{};
  asio::post(impl_->io, [impl = impl_] { impl->fail("destroyed"); });
}

void WsClient::connect() { impl_->connect(); }

void WsClient::send_text(std::string payload) {
  asio::post(impl_->io, [impl = impl_, p = std::move(payload)] {
    impl->queue(WsOpcode::Text, std::as_bytes(std::span<const char>{p.data(), p.size()}));
  });
}

void WsClient::close(std::uint16_t code, std::string reason) {
  asio::post(impl_->io,
             [impl = impl_, code, r = std::move(reason)] { impl->start_close(code, r); });
}

bool WsClient::is_open() const noexcept { return impl_->open.load(); }

WsStats WsClient::stats() const noexcept {
  return WsStats{impl_->n_messages.load(), impl_->n_bytes.load(), impl_->n_pings.load(),
                 impl_->n_sent.load()};
}

} // namespace jarvis::network
