// The user data stream session and the listenKey keeper (docs/architecture.md section 14.5)
// against a scripted loopback venue.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/adapter/binance/user_stream.hpp"
#include "jarvis/adapter/binance/user_stream_session.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/ws_client.hpp"
#include "support/ws_test.hpp"

namespace {

namespace binance = jarvis::adapter::binance;
namespace net = jarvis::network;
using jarvis::core::Status;
using jarvis::testsupport::ScriptedHttpsServer;
using jarvis::testsupport::ScriptedWssServer;
using jarvis::testsupport::WsReply;

constexpr std::int64_t kMinute = 60'000'000'000;

bool run_until(net::IoContext& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = std::chrono::milliseconds{5000}) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > end) {
      return false;
    }
    io.restart();
    io.run_for(std::chrono::milliseconds{5});
  }
  return true;
}

// The number after "id": in a subscription request.
std::string id_of(const std::string& m) {
  const std::size_t at = m.find("\"id\":");
  return at == std::string::npos ? std::string{"0"} : m.substr(at + 5, m.find('}', at) - at - 5);
}

std::string confirmed(const std::string& m) { return R"({"result":null,"id":)" + id_of(m) + "}"; }

std::string event(std::string_view key, std::string_view tag) {
  return R"({"stream":")" + std::string{key} +
         R"(","data":{"e":"ACCOUNT_CONFIG_UPDATE","T":1700000000000,"E":1700000000001,"ac":{"s":")" +
         std::string{tag} + R"(","l":20}}})";
}

struct Recorder {
  std::vector<std::string> frames;
  std::vector<std::string> downs;
  int lives = 0;

  binance::UserStreamHandlers handlers() {
    binance::UserStreamHandlers h;
    h.on_live = [this] { ++lives; };
    h.on_down = [this](const std::string& r) { downs.push_back(r); };
    h.on_frame = [this](std::span<const std::byte> f, std::int64_t) {
      frames.emplace_back(reinterpret_cast<const char*>(f.data()), f.size()); // NOLINT
    };
    return h;
  }
};

binance::UserStreamConfig config_for(const ScriptedWssServer& server) {
  binance::UserStreamConfig c;
  c.url = server.url("/private/stream");
  c.tls.ca_file = server.ca_file();
  c.reconnect_initial = std::chrono::milliseconds{20};
  c.reconnect_max = std::chrono::milliseconds{100};
  return c;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the user stream subscribes the listenKey and passes events on") {
    ScriptedWssServer server{1, [](std::size_t, const std::string& m) {
                               std::vector<WsReply> out;
                               if (m.find("\"SUBSCRIBE\"") != std::string::npos) {
                                 out.push_back(WsReply::send(confirmed(m)));
                                 if (m.find("LK1") != std::string::npos) {
                                   out.push_back(WsReply::send(event("LK1", "BTCUSDT")));
                                   out.push_back(WsReply::send(event("LK1", "ETHUSDT")));
                                 }
                               } else if (!m.empty()) {
                                 out.push_back(WsReply::send(confirmed(m)));
                               }
                               return out;
                             }};
    net::IoContext io;
    Recorder rec;
    binance::UserStreamSession session{io, config_for(server), rec.handlers()};
    session.start("LK1");
    REQUIRE(run_until(io, [&] { return rec.frames.size() == 2; }));
    CHECK(session.live());
    CHECK(rec.lives == 1);
    CHECK(server.targets() == std::vector<std::string>{"/private/stream"});

    // The frames are the venue's bytes; the decoder takes the envelope as it is.
    binance::UserReport report;
    std::string error;
    REQUIRE(binance::decode_user_report(rec.frames[1], report, error) == Status::Ok);
    REQUIRE(std::holds_alternative<binance::ConfigReport>(report));

    session.set_listen_key("LK2");
    session.set_listen_key("LK2"); // unchanged: nothing sent
    REQUIRE(run_until(io, [&] { return server.messages().size() == 3; }));
    const auto messages = server.messages();
    CHECK(messages[0].second == R"({"method":"SUBSCRIBE","params":["LK1"],"id":1})");
    CHECK(messages[1].second == R"({"method":"SUBSCRIBE","params":["LK2"],"id":2})");
    CHECK(messages[2].second == R"({"method":"UNSUBSCRIBE","params":["LK1"],"id":3})");
    io.restart();
    io.run_for(std::chrono::milliseconds{50});
    CHECK(session.stats().subscribe_errors == 0);
    CHECK(rec.downs.empty());
    session.stop();
    REQUIRE(run_until(io, [&] { return rec.downs.size() == 1; }));
    CHECK(server.error().empty());
  }

  TEST_CASE("a dropped user stream reconnects and subscribes again; a refusal retries") {
    ScriptedWssServer server{
        3, [](std::size_t conn, const std::string& m) {
          if (m.empty()) {
            return std::vector<WsReply>{};
          }
          if (conn == 0) {
            return std::vector<WsReply>{WsReply::send(confirmed(m)),
                                        WsReply::send(event("LK1", "A")), WsReply::drop()};
          }
          if (conn == 1) {
            return std::vector<WsReply>{WsReply::send(
                R"({"error":{"code":2,"msg":"Invalid request"},"id":)" + id_of(m) + "}")};
          }
          return std::vector<WsReply>{WsReply::send(confirmed(m)),
                                      WsReply::send(event("LK1", "B"))};
        }};
    net::IoContext io;
    Recorder rec;
    binance::UserStreamSession session{io, config_for(server), rec.handlers()};
    session.start("LK1");
    REQUIRE(run_until(io, [&] { return rec.frames.size() == 2; }));
    CHECK(rec.lives == 2);
    CHECK(rec.downs.size() == 1);
    CHECK(session.live());
    const binance::UserStreamStats st = session.stats();
    CHECK(st.connects == 3);
    CHECK(st.subscribe_errors == 1);
    CHECK(rec.frames[0].find("\"A\"") != std::string::npos);
    CHECK(rec.frames[1].find("\"B\"") != std::string::npos);
    CHECK(server.error().empty());
  }

  TEST_CASE("a user stream that goes silent is dropped by the idle deadline and reconnects") {
    // Connection 0 confirms, delivers an event and goes silent without closing. Connection 1
    // is a healthy but quiet stream: it must outlive several idle deadlines because the
    // session's pings are answered.
    ScriptedWssServer server{2, [](std::size_t conn, const std::string& m) {
                               if (m.empty()) {
                                 return std::vector<WsReply>{};
                               }
                               if (conn == 0) {
                                 return std::vector<WsReply>{WsReply::send(confirmed(m)),
                                                             WsReply::send(event("LK1", "A")),
                                                             WsReply::silent()};
                               }
                               return std::vector<WsReply>{WsReply::send(confirmed(m)),
                                                           WsReply::send(event("LK1", "B"))};
                             }};
    net::IoContext io;
    Recorder rec;
    binance::UserStreamConfig config = config_for(server);
    config.idle_timeout = std::chrono::milliseconds{500};
    binance::UserStreamSession session{io, config, rec.handlers()};
    session.start("LK1");
    REQUIRE(run_until(io, [&] { return rec.frames.size() == 2; }));
    REQUIRE(rec.downs.size() == 1);
    CHECK(rec.downs[0].starts_with(net::kWsIdleTimeout));
    CHECK(rec.lives == 2);
    CHECK(session.live());
    CHECK(session.stats().connects == 2);
    io.restart();
    io.run_for(std::chrono::milliseconds{1600}); // three deadlines of silence from the server
    CHECK(rec.downs.size() == 1);
    CHECK(session.live());
    CHECK(session.stats().connects == 2);
    session.stop();
    CHECK(server.error().empty());
  }

  TEST_CASE("a planned rotation overlaps two connections and passes each event on once") {
    ScriptedWssServer server{2, [](std::size_t, const std::string& m) {
                               return m.empty() ? std::vector<WsReply>{}
                                                : std::vector<WsReply>{WsReply::send(confirmed(m))};
                             }};
    net::IoContext io;
    Recorder rec;
    binance::UserStreamConfig config = config_for(server);
    config.rotate_after = std::chrono::milliseconds{200};
    config.rotation_overlap = std::chrono::milliseconds{1500};
    binance::UserStreamSession session{io, config, rec.handlers()};
    session.start("LK1");
    REQUIRE(run_until(io, [&] { return session.stats().rotations == 1; }));

    // During the overlap the venue sends each event on both connections.
    server.push(0, WsReply::send(event("LK1", "A")));
    server.push(1, WsReply::send(event("LK1", "A")));
    server.push(0, WsReply::send(event("LK1", "B"))); // only the old connection had it
    server.push(1, WsReply::send(event("LK1", "C")));
    REQUIRE(
        run_until(io, [&] { return rec.frames.size() == 3 && session.stats().duplicates == 1; }));
    REQUIRE(run_until(io, [&] { return server.served() == 1; })); // the old one closed
    server.push(1, WsReply::send(event("LK1", "D")));
    REQUIRE(run_until(io, [&] { return rec.frames.size() == 4; }));
    CHECK(rec.downs.empty());
    CHECK(rec.lives == 1);
    CHECK(session.live());
    int a = 0;
    for (const std::string& f : rec.frames) {
      a += f.find("\"A\"") != std::string::npos ? 1 : 0;
    }
    CHECK(a == 1);
    session.stop();
    CHECK(server.error().empty());
  }

  TEST_CASE("the listenKey keeper keeps the key alive and renews it") {
    std::vector<std::string> keys{"K1", "K1", "K2"};
    std::size_t creates = 0;
    bool keepalive_ok = true;
    bool create_ok = true;
    binance::ListenKeyOps ops;
    ops.create = [&](std::string& key, std::string& error) {
      if (!create_ok) {
        error = "down";
        return Status::IoError;
      }
      key = keys[std::min(creates++, keys.size() - 1)];
      return Status::Ok;
    };
    ops.keep_alive = [&](std::string& error) {
      if (!keepalive_ok) {
        error = "-1125 This listenKey does not exist.";
        return Status::InvalidArgument;
      }
      return Status::Ok;
    };
    binance::ListenKeyKeeper keeper{ops};
    std::string error;
    REQUIRE(keeper.start(0, error) == Status::Ok);
    CHECK(keeper.key() == "K1");
    CHECK_FALSE(keeper.tick(29 * kMinute));
    CHECK(keeper.stats().keepalives == 0);
    CHECK_FALSE(keeper.tick(30 * kMinute));
    CHECK(keeper.stats().keepalives == 1);

    keepalive_ok = false; // the venue still has the key: create hands back the same one
    CHECK_FALSE(keeper.tick(60 * kMinute));
    CHECK(keeper.key() == "K1");
    CHECK(keeper.stats().creates == 2);
    CHECK(keeper.stats().changes == 0);

    keeper.expired();
    CHECK(keeper.tick(61 * kMinute)); // a new key: the session must subscribe it
    CHECK(keeper.key() == "K2");
    CHECK(keeper.stats().changes == 1);

    keeper.expired();
    create_ok = false;
    CHECK_FALSE(keeper.tick(62 * kMinute));
    CHECK(keeper.last_error() == "down");
    const std::uint64_t failures = keeper.stats().failures;
    CHECK_FALSE(keeper.tick(62 * kMinute + 1)); // waits retry_after before trying again
    CHECK(keeper.stats().failures == failures);
    create_ok = true;
    CHECK_FALSE(keeper.tick(63 * kMinute)); // same key K2 again
    CHECK(keeper.stats().creates == 4);
  }

  TEST_CASE("the keeper drives the REST listenKey endpoints") {
    const auto http = [](std::string_view body) {
      return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
             std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
    };
    ScriptedHttpsServer server{
        {http(R"({"listenKey":"pqia91ma19a5s61cv6a81va65sdf19v8a65a1"})"), http("{}")}};
    binance::RestConfig config;
    config.base_url = server.url();
    config.tls.ca_file = server.ca_file();
    config.api_key = "KEY1";
    binance::RestClient rest{config};
    binance::ListenKeyKeeper keeper{binance::rest_listen_keys(rest)};
    std::string error;
    REQUIRE(keeper.start(0, error) == Status::Ok);
    CHECK(keeper.key() == "pqia91ma19a5s61cv6a81va65sdf19v8a65a1");
    CHECK_FALSE(keeper.tick(30 * kMinute));
    server.join();
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].starts_with("POST /fapi/v1/listenKey"));
    CHECK(requests[1].starts_with("PUT /fapi/v1/listenKey"));
    CHECK(requests[1].find("X-MBX-APIKEY: KEY1") != std::string::npos);
    CHECK(keeper.stats().keepalives == 1);
    CHECK(server.error().empty());
  }
}
