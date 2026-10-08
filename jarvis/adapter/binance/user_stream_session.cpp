#include "jarvis/adapter/binance/user_stream_session.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <string_view>
#include <utility>

#include <simdjson.h>

#include "jarvis/adapter/binance/rest_client.hpp"
#include "jarvis/network/backoff.hpp"
#include "jarvis/network/timer.hpp"
#include "jarvis/network/ws_client.hpp"

namespace jarvis::adapter::binance {

namespace {

using core::Status;
namespace dom = simdjson::dom;

constexpr std::size_t kOverlapFrames = 4096; // frames remembered for duplicates during a rotation

std::string_view view_of(std::span<const std::byte> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()}; // NOLINT(*-reinterpret-cast)
}

std::string subscription(std::string_view method, std::string_view key, std::uint64_t id) {
  std::string out = R"({"method":")";
  out += method;
  out += R"(","params":[")";
  out += key;
  out += R"("],"id":)";
  out += std::to_string(id);
  out += '}';
  return out;
}

// A control answer ({"result":null,"id":7} or {"error":{...},"id":7}); false for an event.
bool control_answer(std::string_view frame, std::uint64_t& id, bool& refused, std::string& why) {
  if (frame.starts_with(R"({"stream")")) {
    return false; // the combined-stream envelope of an event
  }
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{frame};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS ||
      doc["id"].get_uint64().get(id) != simdjson::SUCCESS) {
    return false;
  }
  dom::element err;
  refused = doc["error"].get(err) == simdjson::SUCCESS;
  if (refused) {
    std::string_view msg;
    std::int64_t code = 0;
    if (err["code"].get_int64().get(code) != simdjson::SUCCESS) {
      code = 0;
    }
    if (err["msg"].get_string().get(msg) != simdjson::SUCCESS) {
      msg = {};
    }
    why = std::to_string(code) + " " + std::string{msg};
  }
  return true;
}

} // namespace

struct UserStreamSession::Impl : std::enable_shared_from_this<Impl> {
  enum class State : std::uint8_t { Connecting, Subscribing, Live, Retiring };

  struct Conn {
    std::shared_ptr<network::WsClient> ws;
    State state = State::Connecting;
    std::uint64_t subscribe_id = 0;
  };

  Impl(network::IoContext& io_, UserStreamConfig config_, UserStreamHandlers handlers_)
      : io{io_}, config{std::move(config_)}, handlers{std::move(handlers_)}, reconnect_timer{io_},
        rotate_timer{io_}, subscribe_timer{io_}, retire_timer{io_},
        backoff{config.reconnect_initial, config.reconnect_max} {}

  network::IoContext& io;
  UserStreamConfig config;
  UserStreamHandlers handlers;
  UserStreamStats stats;
  network::Timer reconnect_timer;
  network::Timer rotate_timer;
  network::Timer subscribe_timer;
  network::Timer retire_timer;
  network::Backoff backoff;
  std::map<std::uint64_t, Conn> conns;
  std::deque<std::pair<std::uint64_t, std::string>> recent; // (connection, frame) in an overlap
  std::string key;
  std::uint64_t next_gen = 0;
  std::uint64_t next_id = 0;
  std::uint64_t primary = 0; // the live connection; 0: not live
  bool stopped = true;
  bool announced = false;

  template <typename Fn> std::function<void()> guarded(Fn fn) {
    return [weak = weak_from_this(), fn] {
      if (const auto self = weak.lock()) {
        fn(*self);
      }
    };
  }

  void open_connection() {
    if (stopped) {
      return;
    }
    const std::uint64_t gen = ++next_gen;
    const std::weak_ptr<Impl> weak = weak_from_this();
    network::WsHandlers h;
    h.on_open = [weak, gen] {
      if (const auto self = weak.lock()) {
        self->on_open(gen);
      }
    };
    h.on_message = [weak, gen](network::WsOpcode, std::span<const std::byte> payload,
                               std::int64_t recv_ns) {
      if (const auto self = weak.lock()) {
        self->on_message(gen, payload, recv_ns);
      }
    };
    h.on_close = [weak, gen](const std::string& reason) {
      if (const auto self = weak.lock()) {
        self->on_close(gen, reason);
      }
    };
    network::WsConfig wc;
    wc.url = config.url;
    wc.tls = config.tls;
    wc.idle_timeout = config.idle_timeout;
    Conn& c = conns[gen];
    c.ws = std::make_shared<network::WsClient>(io, std::move(wc), std::move(h));
    ++stats.connects;
    c.ws->connect();
  }

  void on_open(std::uint64_t gen) {
    const auto it = conns.find(gen);
    if (it == conns.end()) {
      return;
    }
    it->second.state = State::Subscribing;
    it->second.subscribe_id = ++next_id;
    it->second.ws->send_text(subscription("SUBSCRIBE", key, it->second.subscribe_id));
    subscribe_timer.after(config.subscribe_timeout, guarded([gen](Impl& self) {
                            const auto c = self.conns.find(gen);
                            if (c != self.conns.end() && c->second.state == State::Subscribing) {
                              c->second.ws->close(1000, "subscribe timeout");
                            }
                          }));
  }

  void on_message(std::uint64_t gen, std::span<const std::byte> payload, std::int64_t recv_ns) {
    const std::string_view frame = view_of(payload);
    std::uint64_t id = 0;
    bool refused = false;
    std::string why;
    if (control_answer(frame, id, refused, why)) {
      on_answer(gen, id, refused, why);
      return;
    }
    if (conns.size() > 1 && duplicate(gen, frame)) {
      ++stats.duplicates;
      return;
    }
    ++stats.frames;
    if (handlers.on_frame) {
      handlers.on_frame(payload, recv_ns);
    }
  }

  // During an overlap: true when the other connection already passed this frame on.
  bool duplicate(std::uint64_t gen, std::string_view frame) {
    const auto seen = std::find_if(recent.begin(), recent.end(), [&](const auto& r) {
      return r.first != gen && r.second == frame;
    });
    if (seen != recent.end()) {
      recent.erase(seen);
      return true;
    }
    recent.emplace_back(gen, frame);
    if (recent.size() > kOverlapFrames) {
      recent.pop_front();
    }
    return false;
  }

  void on_answer(std::uint64_t gen, std::uint64_t id, bool refused, const std::string& why) {
    const auto it = conns.find(gen);
    if (it == conns.end() || id != it->second.subscribe_id) {
      if (refused) {
        ++stats.subscribe_errors; // a later key change or an unsubscribe
      }
      return;
    }
    if (refused) {
      ++stats.subscribe_errors;
      it->second.ws->close(1000, "subscribe refused: " + why);
      return;
    }
    it->second.state = State::Live;
    backoff.reset();
    const std::uint64_t previous = primary;
    primary = gen;
    if (previous != 0 && previous != gen) {
      if (const auto old = conns.find(previous); old != conns.end()) {
        old->second.state = State::Retiring;
        ++stats.rotations;
        retire_timer.after(config.rotation_overlap, guarded([previous](Impl& self) {
                             if (const auto c = self.conns.find(previous); c != self.conns.end()) {
                               c->second.ws->close(1000, "rotated");
                             }
                           }));
      }
    }
    rotate_timer.after(config.rotate_after, guarded([](Impl& self) { self.rotate(); }));
    if (!announced) {
      announced = true;
      if (handlers.on_live) {
        handlers.on_live();
      }
    }
  }

  void rotate() {
    if (stopped) {
      return;
    }
    if (conns.size() > 1) { // a connection is still opening or retiring
      rotate_timer.after(backoff.next(), guarded([](Impl& self) { self.rotate(); }));
      return;
    }
    open_connection();
  }

  void on_close(std::uint64_t gen, const std::string& reason) {
    const auto it = conns.find(gen);
    if (it == conns.end()) {
      return;
    }
    const State state = it->second.state;
    io.post([ws = std::move(it->second.ws)] { static_cast<void>(ws); });
    conns.erase(it);
    if (conns.size() <= 1) {
      recent.clear();
    }
    if (gen == primary) {
      primary = 0;
      for (auto& [g, c] : conns) {
        if (c.state == State::Live || c.state == State::Retiring) {
          c.state = State::Live; // took every event since its subscription
          primary = g;
        }
      }
    }
    if (primary == 0 && announced) {
      announced = false;
      if (handlers.on_down) {
        handlers.on_down(reason);
      }
    }
    if (stopped) {
      return;
    }
    const bool opening = std::any_of(conns.begin(), conns.end(), [](const auto& c) {
      return c.second.state == State::Connecting || c.second.state == State::Subscribing;
    });
    if (primary == 0 && !opening) {
      reconnect_timer.after(backoff.next(), guarded([](Impl& self) { self.open_connection(); }));
    } else if (primary != 0 && (state == State::Connecting || state == State::Subscribing)) {
      rotate_timer.after(backoff.next(), guarded([](Impl& self) { self.rotate(); }));
    }
  }

  void change_key(std::string next) {
    if (next == key) {
      return;
    }
    const std::string old = std::exchange(key, std::move(next));
    for (auto& [gen, c] : conns) {
      if (c.state == State::Connecting) {
        continue; // subscribes the new key when it opens
      }
      c.ws->send_text(subscription("SUBSCRIBE", key, ++next_id));
      c.ws->send_text(subscription("UNSUBSCRIBE", old, ++next_id));
    }
  }

  void shut_down() {
    stopped = true;
    reconnect_timer.cancel();
    rotate_timer.cancel();
    subscribe_timer.cancel();
    retire_timer.cancel();
    for (auto& [gen, c] : conns) {
      c.ws->close(1000, "stopped");
    }
  }
};

UserStreamSession::UserStreamSession(network::IoContext& io, UserStreamConfig config,
                                     UserStreamHandlers handlers)
    : impl_{std::make_shared<Impl>(io, std::move(config), std::move(handlers))} {}

UserStreamSession::~UserStreamSession() { impl_->shut_down(); }

void UserStreamSession::start(std::string listen_key) {
  if (!impl_->stopped) {
    return;
  }
  impl_->key = std::move(listen_key);
  impl_->stopped = false;
  impl_->open_connection();
}

void UserStreamSession::set_listen_key(std::string listen_key) {
  impl_->change_key(std::move(listen_key));
}

void UserStreamSession::stop() { impl_->shut_down(); }

bool UserStreamSession::live() const noexcept { return impl_->primary != 0; }

UserStreamStats UserStreamSession::stats() const noexcept { return impl_->stats; }

ListenKeyOps rest_listen_keys(RestClient& rest) {
  ListenKeyOps ops;
  ops.create = [&rest](std::string& key, std::string& error) {
    return rest.create_listen_key(key, error);
  };
  ops.keep_alive = [&rest](std::string& error) { return rest.keep_alive_listen_key(error); };
  return ops;
}

ListenKeyKeeper::ListenKeyKeeper(ListenKeyOps ops, ListenKeyKeeperConfig config)
    : ops_{std::move(ops)}, config_{config} {}

Status ListenKeyKeeper::start(std::int64_t now_ns, std::string& error) {
  const Status s = ops_.create(key_, error);
  if (!core::ok(s)) {
    ++stats_.failures;
    error_ = error;
    return s;
  }
  ++stats_.creates;
  next_ns_ = now_ns + std::chrono::nanoseconds{config_.keepalive_every}.count();
  return Status::Ok;
}

bool ListenKeyKeeper::tick(std::int64_t now_ns) {
  if (now_ns < next_ns_) {
    return false;
  }
  if (expired_) {
    return renew(now_ns);
  }
  if (core::ok(ops_.keep_alive(error_))) {
    ++stats_.keepalives;
    next_ns_ = now_ns + std::chrono::nanoseconds{config_.keepalive_every}.count();
    return false;
  }
  ++stats_.failures;
  return renew(now_ns);
}

bool ListenKeyKeeper::renew(std::int64_t now_ns) {
  std::string key;
  if (!core::ok(ops_.create(key, error_))) {
    ++stats_.failures;
    next_ns_ = now_ns + std::chrono::nanoseconds{config_.retry_after}.count();
    return false; // expired_ stays set: the next tick tries again
  }
  ++stats_.creates;
  expired_ = false;
  next_ns_ = now_ns + std::chrono::nanoseconds{config_.keepalive_every}.count();
  if (key == key_) {
    return false;
  }
  ++stats_.changes;
  key_ = std::move(key);
  return true;
}

} // namespace jarvis::adapter::binance
