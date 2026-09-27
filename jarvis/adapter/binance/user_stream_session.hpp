#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "jarvis/core/status.hpp"
#include "jarvis/network/io.hpp"

// The user data stream (docs/architecture.md section 14.5).
//
// UserStreamSession runs on the ud-io thread's IoContext; every member is called on that thread.
// It connects to the private route, subscribes the listenKey as a stream, and hands each event
// frame on unparsed (the caller records and decodes it). It is live once the venue confirms the
// subscription: from then on no event is missed until on_down, which is where reconciliation
// starts over (section 15).
//
// The venue closes connections after 24 hours. Before that the session opens a second
// connection and subscribes it; both stay open for an overlap, during which an event arriving
// on both is passed on once (the frames are byte-identical), then the old one closes. A planned
// rotation therefore loses nothing and needs no reconciliation.
//
// ListenKeyKeeper owns the key: it creates it, keeps it alive every 30 minutes (the key lapses
// after 60), and makes a new one when a keepalive fails or the stream reports listenKeyExpired.
// Its calls block (REST), so it runs on a thread that may block (admin), and a changed key goes
// to the session with set_listen_key.

namespace jarvis::adapter::binance {

class RestClient;

struct UserStreamConfig {
  std::string url = "wss://fstream.binance.com/private/stream";
  network::TlsOptions tls;
  std::chrono::milliseconds subscribe_timeout{10'000};
  std::chrono::milliseconds reconnect_initial{500};
  std::chrono::milliseconds reconnect_max{30'000};
  std::chrono::milliseconds rotate_after{std::chrono::hours{23}};
  std::chrono::milliseconds rotation_overlap{2'000};
};

struct UserStreamHandlers {
  std::function<void()> on_live;
  std::function<void(const std::string& reason)> on_down;
  // One event frame ({"stream":"<listenKey>","data":{...}}), borrowed for the call.
  std::function<void(std::span<const std::byte> frame, std::int64_t recv_ns)> on_frame;
};

struct UserStreamStats {
  std::uint64_t frames = 0;
  std::uint64_t duplicates = 0; // dropped during a rotation's overlap
  std::uint64_t connects = 0;
  std::uint64_t rotations = 0;
  std::uint64_t subscribe_errors = 0;
};

class UserStreamSession {
public:
  UserStreamSession(network::IoContext& io, UserStreamConfig config, UserStreamHandlers handlers);
  ~UserStreamSession();
  UserStreamSession(const UserStreamSession&) = delete;
  UserStreamSession& operator=(const UserStreamSession&) = delete;

  void start(std::string listen_key);
  // A new key from the keeper: subscribed on every open connection, the old one unsubscribed.
  void set_listen_key(std::string listen_key);
  void stop();

  [[nodiscard]] bool live() const noexcept;
  [[nodiscard]] UserStreamStats stats() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// The listenKey endpoints the keeper calls; rest_listen_keys adapts a RestClient.
struct ListenKeyOps {
  std::function<core::Status(std::string& key, std::string& error)> create;
  std::function<core::Status(std::string& error)> keep_alive;
};

[[nodiscard]] ListenKeyOps rest_listen_keys(RestClient& rest);

struct ListenKeyKeeperConfig {
  std::chrono::milliseconds keepalive_every{std::chrono::minutes{30}};
  std::chrono::milliseconds retry_after{std::chrono::seconds{10}};
};

struct ListenKeyStats {
  std::uint64_t creates = 0;
  std::uint64_t keepalives = 0;
  std::uint64_t failures = 0;
  std::uint64_t changes = 0; // the venue handed out a different key
};

class ListenKeyKeeper {
public:
  explicit ListenKeyKeeper(ListenKeyOps ops, ListenKeyKeeperConfig config = {});

  // Creates the key; `now_ns` is any monotonic clock the later ticks share.
  [[nodiscard]] core::Status start(std::int64_t now_ns, std::string& error);
  // Keeps the key alive when due. A failed keepalive, or an expiry reported before, asks the
  // venue for a key (which returns the live key when there still is one, extending it). True
  // when the key changed.
  [[nodiscard]] bool tick(std::int64_t now_ns);
  void expired() noexcept { // the stream said listenKeyExpired: renew at the next tick
    expired_ = true;
    next_ns_ = 0;
  }

  [[nodiscard]] const std::string& key() const noexcept { return key_; }
  [[nodiscard]] const std::string& last_error() const noexcept { return error_; }
  [[nodiscard]] ListenKeyStats stats() const noexcept { return stats_; }

private:
  bool renew(std::int64_t now_ns);

  ListenKeyOps ops_;
  ListenKeyKeeperConfig config_;
  std::string key_;
  std::string error_;
  std::int64_t next_ns_ = 0;
  bool expired_ = false;
  ListenKeyStats stats_;
};

} // namespace jarvis::adapter::binance
