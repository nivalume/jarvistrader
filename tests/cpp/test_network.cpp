// The network layer (docs/architecture.md section 13): WebSocket framing against RFC 6455, HTTP
// response parsing, signing against published vectors, and the TLS clients against loopback
// servers with a certificate made at test time.

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <doctest/doctest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include "jarvis/core/status.hpp"
#include "jarvis/network/backoff.hpp"
#include "jarvis/network/crypto.hpp"
#include "jarvis/network/http.hpp"
#include "jarvis/network/https_client.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/signer.hpp"
#include "jarvis/network/url.hpp"
#include "jarvis/network/waker.hpp"
#include "jarvis/network/ws_client.hpp"
#include "jarvis/network/ws_frame.hpp"
#include "support/tls_test.hpp"
#include "support/ws_test.hpp"

namespace {

namespace net = jarvis::network;
using jarvis::core::Status;
using jarvis::testsupport::header_value;
using jarvis::testsupport::make_certificate;
using jarvis::testsupport::read_head;
using jarvis::testsupport::TempDir;

std::vector<std::byte> bytes(std::initializer_list<unsigned> v) {
  std::vector<std::byte> out;
  for (const unsigned b : v) {
    out.push_back(static_cast<std::byte>(b));
  }
  return out;
}

std::span<const std::byte> as_bytes(std::string_view s) {
  return std::as_bytes(std::span<const char>{s.data(), s.size()});
}

std::string text_of(std::span<const std::byte> b) {
  return std::string{reinterpret_cast<const char*>(b.data()), b.size()}; // NOLINT
}

std::vector<std::byte> server_frame(unsigned first, std::string_view payload) {
  return jarvis::testsupport::ws_server_frame(first, payload);
}

template <typename Stream> std::pair<unsigned, std::string> read_client_frame(Stream& s) {
  return jarvis::testsupport::read_ws_client_frame(s);
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("a waker ends another thread's wait for one handler") {
    using Clock = std::chrono::steady_clock;
    net::IoContext io;
    net::Waker waker{io};
    waker.notify(); // nothing before start
    std::string error;
    REQUIRE(waker.start(error) == jarvis::core::Status::Ok);
    // Notifications before the wait are not lost; several collapse into one handler.
    waker.notify();
    waker.notify();
    CHECK(io.run_one_for(std::chrono::milliseconds{5'000}) == 1);
    CHECK(io.poll() == 0);

    std::atomic<bool> waiting{false};
    std::size_t handlers = 0;
    Clock::duration waited{};
    std::thread loop{[&] {
      waiting = true;
      const auto t0 = Clock::now();
      handlers = io.run_one_for(std::chrono::milliseconds{5'000});
      waited = Clock::now() - t0;
    }};
    while (!waiting) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    waker.notify();
    loop.join();
    CHECK(handlers == 1);
    CHECK(waited < std::chrono::seconds{4});
    waker.stop();
  }

  TEST_CASE("URLs split into scheme, host, port and target") {
    net::Endpoint e;
    REQUIRE(net::parse_url("wss://fstream.binance.com/stream?streams=a/b", e) == Status::Ok);
    CHECK(e.scheme == "wss");
    CHECK(e.host == "fstream.binance.com");
    CHECK(e.port == 443);
    CHECK(e.target == "/stream?streams=a/b");
    CHECK(e.tls());
    REQUIRE(net::parse_url("http://127.0.0.1:8080", e) == Status::Ok);
    CHECK(e.port == 8080);
    CHECK(e.target == "/");
    CHECK_FALSE(e.tls());
    REQUIRE(net::parse_url("ws://h?x=1", e) == Status::Ok);
    CHECK(e.target == "/?x=1");
    CHECK(net::parse_url("ftp://h", e) == Status::InvalidArgument);
    CHECK(net::parse_url("wss://h:99999/", e) == Status::ParseError);
    CHECK(net::parse_url("nohost", e) == Status::ParseError);
  }

  TEST_CASE("the opening handshake follows RFC 6455 section 1.3") {
    CHECK(net::ws_accept_for("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    const std::array<std::uint8_t, 16> nonce{};
    CHECK(net::ws_key_from(nonce) == "AAAAAAAAAAAAAAAAAAAAAA==");
    const std::array<net::HeaderField, 1> extra{{{"X-Test", "1"}}};
    const std::string req =
        net::ws_upgrade_request("example.com", 443, true, "/chat", "KEY", extra);
    CHECK(req == "GET /chat HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Key: KEY\r\n"
                 "Sec-WebSocket-Version: 13\r\nX-Test: 1\r\n\r\n");
    CHECK(net::ws_upgrade_request("h", 8443, true, "/", "K").find("Host: h:8443\r\n") !=
          std::string::npos);

    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Accept: "
                           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    std::size_t consumed = 0;
    std::string error;
    CHECK(net::ws_parse_upgrade(ok + "\x81", key, consumed, error) == Status::Ok);
    CHECK(consumed == ok.size());
    CHECK(net::ws_parse_upgrade(ok.substr(0, 40), key, consumed, error) == Status::Truncated);
    std::string wrong = ok;
    wrong.replace(wrong.find("s3pP"), 4, "AAAA");
    CHECK(net::ws_parse_upgrade(wrong, key, consumed, error) == Status::InvalidArgument);
    CHECK(error.find("Accept") != std::string::npos);
    CHECK(net::ws_parse_upgrade("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", key,
                                consumed, error) == Status::InvalidArgument);
    CHECK(error.find("403") != std::string::npos);
  }

  TEST_CASE("client frames are masked as in RFC 6455 section 5.7") {
    std::vector<std::byte> out;
    net::ws_encode(net::WsOpcode::Text, as_bytes("Hello"), {0x37, 0xfa, 0x21, 0x3d}, out);
    CHECK(out == bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));
    out.clear();
    const std::string medium(300, 'x');
    net::ws_encode(net::WsOpcode::Binary, as_bytes(medium), {0, 0, 0, 0}, out);
    CHECK(out.size() == 2 + 2 + 4 + 300);
    CHECK(out[1] == std::byte{0xFE});
    CHECK(out[2] == std::byte{0x01});
    CHECK(out[3] == std::byte{0x2C});
    out.clear();
    const std::string large(70000, 'y');
    net::ws_encode(net::WsOpcode::Binary, as_bytes(large), {1, 2, 3, 4}, out);
    CHECK(out.size() == 2 + 8 + 4 + 70000);
    CHECK(out[1] == std::byte{0xFF});
  }

  TEST_CASE("server frames decode in place; fragments reassemble; partial input waits") {
    std::vector<std::byte> stream;
    const auto append = [&stream](const std::vector<std::byte>& f) {
      stream.insert(stream.end(), f.begin(), f.end());
    };
    append(bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f})); // "Hello"
    append(bytes({0x01, 0x03, 0x48, 0x65, 0x6c}));             // "Hel"...
    append(bytes({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f})); // a ping in between
    append(bytes({0x80, 0x02, 0x6c, 0x6f}));                   // ..."lo"
    append(server_frame(0x82, std::string(256, 'b')));         // 16-bit length
    append(server_frame(0x81, std::string(70000, 'c')));       // 64-bit length
    append(bytes({0x88, 0x02, 0x03, 0xe8}));                   // close 1000

    // Whole buffer at once.
    net::WsDecoder d;
    std::vector<net::WsMessage> out;
    std::size_t consumed = 0;
    REQUIRE(d.decode(stream, consumed, out) == Status::Ok);
    CHECK(consumed == stream.size());
    REQUIRE(out.size() == 6);
    CHECK(text_of(out[0].payload) == "Hello");
    CHECK(out[1].opcode == net::WsOpcode::Ping);
    CHECK(out[2].opcode == net::WsOpcode::Text);
    CHECK(text_of(out[2].payload) == "Hello"); // reassembled
    CHECK(out[3].payload.size() == 256);
    CHECK(out[4].payload.size() == 70000);
    CHECK(out[5].opcode == net::WsOpcode::Close);

    // One byte at a time, keeping the unconsumed tail as a real reader does.
    net::WsDecoder slow;
    std::vector<std::byte> pending;
    std::vector<std::string> texts;
    std::size_t controls = 0;
    for (const std::byte b : stream) {
      pending.push_back(b);
      REQUIRE(slow.decode(pending, consumed, out) == Status::Ok);
      for (const net::WsMessage& m : out) {
        if (m.opcode == net::WsOpcode::Text || m.opcode == net::WsOpcode::Binary) {
          texts.push_back(text_of(m.payload));
        } else {
          ++controls;
        }
      }
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    CHECK(pending.empty());
    CHECK(texts.size() == 4);
    CHECK(texts[1] == "Hello");
    CHECK(controls == 2);
  }

  TEST_CASE("protocol violations are errors") {
    const auto error_of = [](const std::vector<std::byte>& frame) {
      net::WsDecoder d{1024};
      std::vector<net::WsMessage> out;
      std::size_t consumed = 0;
      const Status s = d.decode(frame, consumed, out);
      return std::pair{s, d.error()};
    };
    CHECK(error_of(bytes({0x81, 0x85, 1, 2, 3, 4, 0, 0, 0, 0, 0})).first == Status::ParseError);
    CHECK(error_of(bytes({0xC1, 0x00})).second.find("reserved") != std::string::npos);
    CHECK(error_of(bytes({0x83, 0x00})).second.find("opcode") != std::string::npos);
    CHECK(error_of(bytes({0x09, 0x00})).first == Status::ParseError); // fragmented ping
    CHECK(error_of(server_frame(0x89, std::string(126, 'p'))).first == Status::ParseError);
    CHECK(error_of(bytes({0x80, 0x00})).second.find("continuation") != std::string::npos);
    std::vector<std::byte> twice = bytes({0x01, 0x01, 0x41});
    const std::vector<std::byte> again = bytes({0x81, 0x01, 0x42});
    twice.insert(twice.end(), again.begin(), again.end());
    CHECK(error_of(twice).first == Status::ParseError);
    CHECK(error_of(server_frame(0x82, std::string(2000, 'z'))).first == Status::CapacityExceeded);
    CHECK(error_of(bytes({0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0})).first == Status::ParseError);
  }

  TEST_CASE("HTTP responses: Content-Length, chunked, keep-alive, back to back") {
    const std::string first = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Content-Length: 11\r\nX-MBX-USED-WEIGHT-1M: 7\r\n\r\n{\"ok\":true}";
    const std::string second = "HTTP/1.1 400 Bad Request\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "4\r\n{\"co\r\na\r\nde\":-1121}\r\n0\r\nX-Trailer: 1\r\n\r\n";
    const std::string both = first + second;
    net::HttpResponseParser p;
    std::size_t consumed = 0;
    REQUIRE(p.feed(both, consumed) == Status::Ok);
    CHECK(consumed == first.size());
    CHECK(p.response().status == 200);
    CHECK(p.response().body == "{\"ok\":true}");
    CHECK(p.response().header("x-mbx-used-weight-1m") == std::optional<std::string_view>{"7"});
    CHECK(p.keep_alive());
    std::size_t rest = 0;
    REQUIRE(p.feed(std::string_view{both}.substr(consumed), rest) == Status::Ok);
    CHECK(consumed + rest == both.size());
    CHECK(p.response().status == 400);
    CHECK(p.response().body == "{\"code\":-1121}");

    // Byte by byte.
    net::HttpResponseParser slow;
    Status s = Status::Truncated;
    for (std::size_t i = 0; i < second.size(); ++i) {
      std::size_t used = 0;
      s = slow.feed(std::string_view{second}.substr(i, 1), used);
      CHECK(used == 1);
      if (s == Status::Ok) {
        CHECK(i == second.size() - 1);
      }
    }
    CHECK(s == Status::Ok);
    CHECK(slow.response().body == "{\"code\":-1121}");

    net::HttpResponseParser closing;
    REQUIRE(closing.feed("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc", consumed) ==
            Status::Truncated);
    CHECK_FALSE(closing.keep_alive());
    CHECK(closing.finish() == Status::Ok);
    CHECK(closing.response().body == "abc");
    net::HttpResponseParser bad;
    CHECK(bad.feed("HTXP/1.1 200\r\n\r\n", consumed) == Status::ParseError);
    // Conflicting lengths are ambiguous framing; equal duplicates are accepted.
    net::HttpResponseParser twice;
    CHECK(twice.feed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 2\r\n\r\nabc",
                     consumed) == Status::ParseError);
    net::HttpResponseParser same;
    CHECK(same.feed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\ncontent-length: 3\r\n\r\nabc",
                    consumed) == Status::Ok);
    // Header values are token lists, not substrings.
    net::HttpResponseParser tokens;
    REQUIRE(tokens.feed("HTTP/1.1 200 OK\r\nConnection: Keep-Alive, Close\r\n"
                        "Transfer-Encoding: gzip , Chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n",
                        consumed) == Status::Ok);
    CHECK(tokens.response().body == "x");
    CHECK_FALSE(tokens.keep_alive());
    net::HttpResponseParser closedish;
    REQUIRE(closedish.feed("HTTP/1.1 200 OK\r\nConnection: closed\r\nContent-Length: 0\r\n\r\n",
                           consumed) == Status::Ok);
    CHECK(closedish.keep_alive());

    const std::array<net::HeaderField, 1> h{{{"X-MBX-APIKEY", "k"}}};
    CHECK(net::http_request("POST", "api.example", "/fapi/v1/listenKey", h) ==
          "POST /fapi/v1/listenKey HTTP/1.1\r\nHost: api.example\r\nConnection: keep-alive\r\n"
          "X-MBX-APIKEY: k\r\nContent-Length: 0\r\n\r\n");
  }

  TEST_CASE("signatures match Binance's HMAC example and RFC 8032 test 1") {
    const net::HmacSha256Signer hmac{
        "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"};
    CHECK(hmac.sign("symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&"
                    "recvWindow=5000&timestamp=1499827319559") ==
          "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
    const std::string pem = "-----BEGIN PRIVATE KEY-----\n"
                            "MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n"
                            "-----END PRIVATE KEY-----\n";
    net::Signer signer;
    std::string error;
    REQUIRE(net::Signer::from_secret(pem, signer, error) == Status::Ok);
    CHECK(signer.ed25519());
    CHECK(signer.sign("") == "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv1"
                             "8FlbviRlUUFDjnoQCw==");
    net::Signer from_hmac;
    REQUIRE(net::Signer::from_secret("secret\n", from_hmac, error) == Status::Ok);
    CHECK_FALSE(from_hmac.ed25519());
    CHECK(from_hmac.sign("x") == net::hex(net::hmac_sha256("secret", "x")));
    net::Signer none;
    CHECK(net::Signer::from_secret("-----BEGIN nonsense", none, error) == Status::ParseError);
  }

  TEST_CASE("backoff grows to its cap with bounded jitter") {
    net::Backoff b{std::chrono::milliseconds{100}, std::chrono::milliseconds{5000}, 7};
    std::int64_t last = 0;
    for (int i = 0; i < 12; ++i) {
      const std::int64_t d = b.next().count();
      CHECK(d >= 80);
      CHECK(d <= 6000);
      if (i < 5) {
        CHECK(d > last * 13 / 10); // roughly doubling early on
      }
      last = d;
    }
    CHECK(last >= 4000);
    b.reset();
    CHECK(b.next().count() <= 120);
  }
}

TEST_SUITE("unit") {
  TEST_CASE("the WebSocket client over TLS: upgrade, messages, ping, echo, close") {
    const TempDir dir;
    make_certificate(dir.file("cert.pem"), dir.file("key.pem"));
    asio::io_context server_io;
    asio::ssl::context server_ssl{asio::ssl::context::tls_server};
    server_ssl.use_certificate_chain_file(dir.file("cert.pem"));
    server_ssl.use_private_key_file(dir.file("key.pem"), asio::ssl::context::pem);
    asio::ip::tcp::acceptor acceptor{server_io, {asio::ip::make_address("127.0.0.1"), 0}};
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::atomic<bool> server_ok{false};
    std::string server_error;
    std::string request_target;
    std::string echoed;
    std::thread server{[&] {
      try {
        asio::ssl::stream<asio::ip::tcp::socket> s{server_io, server_ssl};
        acceptor.accept(s.lowest_layer());
        s.handshake(asio::ssl::stream_base::server);
        const std::string head = read_head(s);
        request_target = head.substr(4, head.find(' ', 4) - 4);
        const std::string accept = net::ws_accept_for(header_value(head, "Sec-WebSocket-Key"));
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                     "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                     accept + "\r\n\r\n";
        std::vector<std::byte> out(response.size());
        std::memcpy(out.data(), response.data(), response.size());
        const auto push = [&out](const std::vector<std::byte>& f) {
          out.insert(out.end(), f.begin(), f.end());
        };
        push(server_frame(0x81, "hello")); // in the same write as the 101
        push(server_frame(0x01, "frag"));
        push(server_frame(0x80, "mented"));
        push(server_frame(0x89, "ping!"));
        push(server_frame(0x82, std::string(100'000, 'z')));
        asio::write(s, asio::buffer(out));
        auto [op1, pong] = read_client_frame(s);
        REQUIRE(op1 == 0xA);
        REQUIRE(pong == "ping!");
        auto [op2, text] = read_client_frame(s);
        REQUIRE(op2 == 0x1);
        echoed = text;
        const std::vector<std::byte> back = server_frame(0x81, "echo:" + text);
        asio::write(s, asio::buffer(back));
        auto [op3, close] = read_client_frame(s);
        REQUIRE(op3 == 0x8);
        const std::vector<std::byte> answer = server_frame(0x88, close);
        asio::write(s, asio::buffer(answer));
        server_ok = true;
      } catch (const std::exception& e) {
        server_error = e.what();
      }
    }};

    net::IoContext io;
    std::vector<std::string> messages;
    std::string closed;
    bool opened = false;
    net::WsConfig config;
    config.url = "wss://localhost:" + std::to_string(port) + "/stream?streams=a@b";
    config.tls.ca_file = dir.file("cert.pem");
    config.receive_buffer = 4096; // grows for the 100 kB message
    net::WsClient* client_ptr = nullptr;
    net::WsHandlers handlers;
    handlers.on_open = [&] { opened = true; };
    handlers.on_message = [&](net::WsOpcode, std::span<const std::byte> payload, std::int64_t) {
      messages.push_back(payload.size() > 64 ? std::to_string(payload.size()) : text_of(payload));
      if (messages.size() == 3) {
        client_ptr->send_text("abc");
      } else if (messages.size() == 4) {
        client_ptr->close();
      }
    };
    handlers.on_close = [&](const std::string& reason) {
      closed = reason;
      io.stop();
    };
    net::WsClient client{io, config, handlers};
    client_ptr = &client;
    client.connect();
    io.run_for(std::chrono::seconds{10});
    server.join();
    CHECK(server_error.empty());
    CHECK(server_ok);
    CHECK(opened);
    CHECK(request_target == "/stream?streams=a@b");
    CHECK(messages == std::vector<std::string>{"hello", "fragmented", "100000", "echo:abc"});
    CHECK(echoed == "abc");
    CHECK(closed.starts_with("closed"));
    CHECK(client.stats().pings == 1);
    CHECK(client.stats().messages == 4);
  }

  TEST_CASE("the WebSocket client reconnects from on_close without stale completions") {
    // Plain ws://: the first connection is dropped without a close frame; the client reconnects
    // from inside on_close, and the aborted operations of the first connection must not touch
    // the second.
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor{server_io, {asio::ip::make_address("127.0.0.1"), 0}};
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string server_error;
    std::thread server{[&] {
      try {
        for (int c = 0; c < 2; ++c) {
          asio::ip::tcp::socket s{server_io};
          acceptor.accept(s);
          const std::string head = read_head(s);
          const std::string response =
              "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Accept: " +
              net::ws_accept_for(header_value(head, "Sec-WebSocket-Key")) + "\r\n\r\n";
          asio::write(s, asio::buffer(response));
          const std::vector<std::byte> hello = server_frame(0x81, c == 0 ? "first" : "second");
          asio::write(s, asio::buffer(hello));
          const auto expect = [](bool ok, const char* what) {
            if (!ok) {
              throw std::runtime_error{what};
            }
          };
          auto [op, text] = read_client_frame(s); // the client's answer
          expect(op == 0x1 && text == (c == 0 ? "got first" : "got second"), "the answer");
          if (c == 0) {
            continue; // a hard drop: no close frame
          }
          auto [op_close, close] = read_client_frame(s);
          expect(op_close == 0x8, "the close");
          const std::vector<std::byte> answer = server_frame(0x88, close);
          asio::write(s, asio::buffer(answer));
        }
      } catch (const std::exception& e) {
        server_error = e.what();
      }
    }};

    net::IoContext io;
    std::vector<std::string> messages;
    std::vector<std::string> closes;
    int opens = 0;
    net::WsConfig config;
    config.url = "ws://127.0.0.1:" + std::to_string(port) + "/ws";
    net::WsClient* client_ptr = nullptr;
    net::WsHandlers handlers;
    handlers.on_open = [&] { ++opens; };
    handlers.on_message = [&](net::WsOpcode, std::span<const std::byte> payload, std::int64_t) {
      messages.push_back(text_of(payload));
      client_ptr->send_text("got " + messages.back());
      if (messages.size() == 2) {
        client_ptr->close();
      }
    };
    handlers.on_close = [&](const std::string& reason) {
      closes.push_back(reason);
      if (closes.size() == 1) {
        client_ptr->connect();
      } else {
        io.stop();
      }
    };
    net::WsClient client{io, config, handlers};
    client_ptr = &client;
    client.connect();
    io.run_for(std::chrono::seconds{10});
    server.join();
    CHECK(server_error.empty());
    CHECK(opens == 2);
    CHECK(messages == std::vector<std::string>{"first", "second"});
    REQUIRE(closes.size() == 2);
    CHECK(closes[0].starts_with("read:"));
    CHECK(closes[1] == "closed");
  }

  TEST_CASE("the WebSocket client fails a connection that goes silent with an idle timeout") {
    // The server accepts, says hello and then neither reads nor writes nor closes: no FIN, no
    // RST, no pong, as after a VM snapshot or fork. Only the client's idle deadline can end it.
    using jarvis::testsupport::ScriptedWssServer;
    using jarvis::testsupport::WsReply;
    for (const bool client_ping : {true, false}) {
      CAPTURE(client_ping);
      ScriptedWssServer server{1, [](std::size_t, const std::string& m) {
                                 std::vector<WsReply> out;
                                 if (m.empty()) {
                                   out.push_back(WsReply::send("hello"));
                                   out.push_back(WsReply::silent());
                                 }
                                 return out;
                               }};
      net::IoContext io;
      net::WsConfig config;
      config.url = server.url("/stream");
      config.tls.ca_file = server.ca_file();
      config.idle_timeout = std::chrono::milliseconds{600};
      config.client_ping = client_ping;
      std::vector<std::string> messages;
      std::vector<std::string> closes;
      bool opened = false;
      net::WsHandlers handlers;
      handlers.on_open = [&] { opened = true; };
      handlers.on_message = [&](net::WsOpcode, std::span<const std::byte> payload, std::int64_t) {
        messages.push_back(text_of(payload));
      };
      handlers.on_close = [&](const std::string& reason) {
        closes.push_back(reason);
        io.stop();
      };
      net::WsClient client{io, config, handlers};
      client.connect();
      const auto started = std::chrono::steady_clock::now();
      io.run_for(std::chrono::seconds{10});
      const auto elapsed = std::chrono::steady_clock::now() - started;
      CHECK(opened);
      CHECK(messages == std::vector<std::string>{"hello"});
      REQUIRE(closes.size() == 1);
      CHECK(closes[0].starts_with(net::kWsIdleTimeout));
      CHECK_FALSE(client.is_open());
      CHECK(client.stats().idle_timeouts == 1);
      CHECK(client.stats().client_pings == (client_ping ? 1U : 0U));
      CHECK(client.stats().pongs == 0);
      // The deadline counts from the last byte received (the hello), not from the ping.
      CHECK(elapsed >= std::chrono::milliseconds{500});
      CHECK(elapsed < std::chrono::seconds{5});
      CHECK((closes[0].find("not answered") != std::string::npos) == client_ping);
      CHECK(server.error().empty());
    }
  }

  TEST_CASE("the WebSocket client keeps a quiet or slow connection that is alive") {
    using jarvis::testsupport::ScriptedWssServer;
    using jarvis::testsupport::WsReply;
    const auto serve = [](std::size_t, const std::string&) { return std::vector<WsReply>{}; };
    constexpr std::chrono::milliseconds kIdle{500};

    // Quiet for 2.5 times the deadline: the client's pings, answered by pongs, keep it open.
    {
      ScriptedWssServer server{1, serve};
      net::IoContext io;
      net::WsConfig config;
      config.url = server.url("/stream");
      config.tls.ca_file = server.ca_file();
      config.idle_timeout = kIdle;
      std::vector<std::string> closes;
      net::WsHandlers handlers;
      handlers.on_close = [&](const std::string& reason) { closes.push_back(reason); };
      net::WsClient client{io, config, handlers};
      client.connect();
      io.run_for(kIdle * 5 / 2);
      CHECK(closes.empty());
      CHECK(client.is_open());
      CHECK(client.stats().client_pings >= 4);
      CHECK(client.stats().pongs >= 4);
      CHECK(client.stats().idle_timeouts == 0);
      client.close();
      io.run_for(std::chrono::seconds{2});
      REQUIRE(closes.size() == 1);
      CHECK(closes[0] == "closed");
      CHECK(server.error().empty());
    }

    // Without client pings, data that keeps coming more often than the deadline is enough.
    {
      ScriptedWssServer server{1, serve};
      net::IoContext io;
      net::WsConfig config;
      config.url = server.url("/stream");
      config.tls.ca_file = server.ca_file();
      config.idle_timeout = kIdle;
      config.client_ping = false;
      std::vector<std::string> closes;
      std::size_t messages = 0;
      net::WsHandlers handlers;
      handlers.on_message = [&](net::WsOpcode, std::span<const std::byte>, std::int64_t) {
        ++messages;
      };
      handlers.on_close = [&](const std::string& reason) { closes.push_back(reason); };
      net::WsClient client{io, config, handlers};
      client.connect();
      const auto end = std::chrono::steady_clock::now() + kIdle * 3;
      auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
      while (std::chrono::steady_clock::now() < end) {
        io.run_for(std::chrono::milliseconds{10});
        if (std::chrono::steady_clock::now() >= next) {
          server.push(0, WsReply::send("tick")); // every 350 ms: slower than a busy stream
          next += std::chrono::milliseconds{350};
        }
      }
      CHECK(closes.empty());
      CHECK(messages >= 3);
      CHECK(client.stats().client_pings == 0);
      CHECK(client.stats().idle_timeouts == 0);
      client.close();
      io.run_for(std::chrono::seconds{2});
      CHECK(closes.size() == 1);
    }

    // The idle deadline is off by default: a silent server does not end the connection.
    {
      ScriptedWssServer server{1, [](std::size_t, const std::string& m) {
                                 std::vector<WsReply> out;
                                 if (m.empty()) {
                                   out.push_back(WsReply::silent());
                                 }
                                 return out;
                               }};
      net::IoContext io;
      net::WsConfig config;
      config.url = server.url("/stream");
      config.tls.ca_file = server.ca_file();
      std::vector<std::string> closes;
      net::WsHandlers handlers;
      handlers.on_close = [&](const std::string& reason) { closes.push_back(reason); };
      net::WsClient client{io, config, handlers};
      client.connect();
      io.run_for(std::chrono::milliseconds{1500});
      CHECK(closes.empty());
      CHECK(client.is_open());
      CHECK(client.stats().client_pings == 0);
    }
  }

  TEST_CASE("the WebSocket client refuses an untrusted certificate") {
    const TempDir dir;
    make_certificate(dir.file("cert.pem"), dir.file("key.pem"));
    asio::io_context server_io;
    asio::ssl::context server_ssl{asio::ssl::context::tls_server};
    server_ssl.use_certificate_chain_file(dir.file("cert.pem"));
    server_ssl.use_private_key_file(dir.file("key.pem"), asio::ssl::context::pem);
    asio::ip::tcp::acceptor acceptor{server_io, {asio::ip::make_address("127.0.0.1"), 0}};
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::thread server{[&] {
      asio::ssl::stream<asio::ip::tcp::socket> s{server_io, server_ssl};
      acceptor.accept(s.lowest_layer());
      asio::error_code ec;
      s.handshake(asio::ssl::stream_base::server, ec); // fails: the client aborts
    }};
    net::IoContext io;
    std::string closed;
    net::WsConfig config;
    config.url = "wss://localhost:" + std::to_string(port) + "/";
    net::WsHandlers handlers;
    handlers.on_close = [&](const std::string& reason) {
      closed = reason;
      io.stop();
    };
    net::WsClient client{io, config, handlers}; // no ca_file: the self-signed cert is unknown
    client.connect();
    io.run_for(std::chrono::seconds{10});
    server.join();
    CHECK(closed.find("TLS handshake") != std::string::npos);
  }

  TEST_CASE("the HTTPS client keeps the connection alive and reconnects after a close") {
    const TempDir dir;
    make_certificate(dir.file("cert.pem"), dir.file("key.pem"));
    asio::io_context server_io;
    asio::ssl::context server_ssl{asio::ssl::context::tls_server};
    server_ssl.use_certificate_chain_file(dir.file("cert.pem"));
    server_ssl.use_private_key_file(dir.file("key.pem"), asio::ssl::context::pem);
    asio::ip::tcp::acceptor acceptor{server_io, {asio::ip::make_address("127.0.0.1"), 0}};
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::vector<std::string> seen;
    int connections = 0;
    std::thread server{[&] {
      for (int c = 0; c < 2; ++c) {
        asio::ssl::stream<asio::ip::tcp::socket> s{server_io, server_ssl};
        acceptor.accept(s.lowest_layer());
        s.handshake(asio::ssl::stream_base::server);
        ++connections;
        const int requests = c == 0 ? 2 : 1;
        for (int r = 0; r < requests; ++r) {
          const std::string head = read_head(s);
          seen.push_back(head.substr(0, head.find("\r\n")));
          std::string response;
          if (r == 0) {
            response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
          } else {
            response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
                       "\r\n3\r\n[1]\r\n0\r\n\r\n";
          }
          asio::write(s, asio::buffer(response));
        }
        // A plain close: a TLS shutdown would wait for the client's close_notify, and the
        // client keeps its last (keep-alive) connection open.
        asio::error_code ec;
        s.lowest_layer().close(ec);
      }
    }};
    net::HttpsConfig config;
    config.base_url = "https://localhost:" + std::to_string(port);
    config.tls.ca_file = dir.file("cert.pem");
    net::HttpsClient client{config};
    net::HttpResponse r;
    std::string error;
    REQUIRE(client.request("GET", "/fapi/v1/ping", {}, {}, r, error) == Status::Ok);
    CHECK(r.body == "{}");
    REQUIRE(client.request("GET", "/fapi/v1/time", {}, {}, r, error) == Status::Ok);
    CHECK(r.body == "[1]");
    REQUIRE(client.request("GET", "/fapi/v1/depth?symbol=BTCUSDT", {}, {}, r, error) == Status::Ok);
    CHECK(r.body == "{}");
    server.join();
    CHECK(connections == 2);
    CHECK(seen == std::vector<std::string>{"GET /fapi/v1/ping HTTP/1.1",
                                           "GET /fapi/v1/time HTTP/1.1",
                                           "GET /fapi/v1/depth?symbol=BTCUSDT HTTP/1.1"});
    net::HttpsConfig nowhere;
    nowhere.base_url = "https://127.0.0.1:1";
    nowhere.timeout = std::chrono::milliseconds{2000};
    net::HttpsClient refused{nowhere};
    CHECK(refused.request("GET", "/", {}, {}, r, error) == Status::IoError);
    CHECK_FALSE(error.empty());
  }
}
