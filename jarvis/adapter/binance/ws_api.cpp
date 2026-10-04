#include "jarvis/adapter/binance/ws_api.hpp"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#include "jarvis/network/backoff.hpp"
#include "jarvis/network/ws_client.hpp"

namespace jarvis::adapter::binance {

namespace {

using core::Status;

std::int64_t system_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string text_of(std::span<const std::byte> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()}; // NOLINT(*-reinterpret-cast)
}

} // namespace

struct WsApiSession::Impl : std::enable_shared_from_this<Impl> {
  enum class State : std::uint8_t { Connecting, LoggingOn, Ready, Draining };

  struct Conn {
    std::shared_ptr<network::WsClient> ws;
    State state = State::Connecting;
  };

  struct Pending {
    std::uint64_t gen = 0;
    std::int64_t deadline_ns = 0;
    bool logon = false;
    bool order = false;
    bool timed_out = false;
    RequestKind kind = RequestKind::Place;
    std::string client_order_id;
    Callback callback;
  };

  Impl(network::IoContext& io_, WsApiConfig config_, WsApiHandlers handlers_)
      : io{io_}, config{std::move(config_)}, handlers{std::move(handlers_)}, timeout_timer{io_},
        reconnect_timer{io_}, rotate_timer{io_},
        backoff{config.reconnect_initial, config.reconnect_max} {}

  network::IoContext& io;
  WsApiConfig config;
  WsApiHandlers handlers;
  WsApiStats stats;
  std::atomic<std::int64_t> offset_ms{0};
  network::Timer timeout_timer;
  network::Timer reconnect_timer;
  network::Timer rotate_timer;
  network::Backoff backoff;
  std::map<std::uint64_t, Conn> conns; // by generation
  std::map<std::string, Pending, std::less<>> pending;
  std::uint64_t next_gen = 0;
  std::uint64_t active = 0; // the connection new requests go to; 0: none is ready
  std::uint64_t seq = 0;
  bool stopped = true;
  bool announced_ready = false;

  [[nodiscard]] std::int64_t venue_ms() const {
    return (config.now_ms ? config.now_ms() : system_ms()) + offset_ms.load();
  }

  [[nodiscard]] bool opening() const {
    return std::any_of(conns.begin(), conns.end(), [](const auto& c) {
      return c.second.state == State::Connecting || c.second.state == State::LoggingOn;
    });
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
        self->on_message(gen, text_of(payload), recv_ns);
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
    if (!config.signer.ed25519()) {
      became_ready(gen);
      return;
    }
    it->second.state = State::LoggingOn;
    const std::string id = next_id();
    Pending p;
    p.gen = gen;
    p.logon = true;
    p.deadline_ns = network::steady_ns() + timeout_ns();
    pending.emplace(id, std::move(p));
    it->second.ws->send_text(
        ws_request(id, "session.logon", {}, venue_ms(), &config.signer, config.api_key));
    arm_timeout();
  }

  void became_ready(std::uint64_t gen) {
    Conn& c = conns[gen];
    c.state = State::Ready;
    backoff.reset();
    const std::uint64_t previous = active;
    active = gen;
    if (previous != 0 && previous != gen) {
      if (const auto old = conns.find(previous); old != conns.end()) {
        old->second.state = State::Draining;
        ++stats.rotations;
        close_if_drained(previous);
      }
    }
    const std::weak_ptr<Impl> weak = weak_from_this();
    rotate_timer.after(config.rotate_after, [weak] {
      if (const auto self = weak.lock()) {
        self->rotate();
      }
    });
    if (!announced_ready) {
      announced_ready = true;
      if (handlers.on_ready) {
        handlers.on_ready();
      }
    }
  }

  void rotate() {
    if (stopped || opening()) {
      return;
    }
    open_connection();
  }

  void close_if_drained(std::uint64_t gen) {
    const auto it = conns.find(gen);
    if (it == conns.end() || it->second.state != State::Draining) {
      return;
    }
    const bool busy = std::any_of(pending.begin(), pending.end(), [gen](const auto& p) {
      return p.second.gen == gen && !p.second.timed_out;
    });
    if (!busy) {
      it->second.ws->close(1000, "rotated");
    }
  }

  void on_message(std::uint64_t gen, const std::string& json, std::int64_t recv_ns) {
    WsAnswer answer;
    std::string error;
    if (!core::ok(decode_ws_answer(
            json, core::UnixNanos{static_cast<std::uint64_t>(venue_ms()) * 1'000'000}, answer,
            error))) {
      ++stats.decode_errors;
      return;
    }
    if (!answer.limits.empty() && handlers.on_limits) {
      handlers.on_limits(answer.limits);
    }
    const auto it = pending.find(answer.id);
    if (it == pending.end()) {
      ++stats.unmatched;
      return;
    }
    Pending p = std::move(it->second);
    pending.erase(it);
    if (p.logon) {
      logon_answer(gen, answer);
      return;
    }
    if (p.timed_out) {
      ++stats.late_answers;
    }
    if (p.order) {
      order_answer(p, answer, recv_ns);
    } else if (!p.timed_out && p.callback) {
      p.callback(Status::Ok, answer, json);
    }
    close_if_drained(p.gen);
  }

  void logon_answer(std::uint64_t gen, const WsAnswer& answer) {
    if (answer.status == 200) {
      became_ready(gen);
      return;
    }
    // A refused logon means the key or its permissions are wrong: reconnecting cannot help.
    const std::string reason =
        "session.logon refused: " + std::to_string(answer.error_code) + " " + answer.error_msg;
    shut_down();
    announced_ready = false;
    if (handlers.on_down) {
      handlers.on_down(reason);
    }
  }

  void order_answer(const Pending& p, const WsAnswer& answer, std::int64_t recv_ns) {
    OrderOutcome o;
    o.request = p.kind;
    o.client_order_id = p.client_order_id;
    o.recv_ns = recv_ns;
    if (answer.status == 200) {
      ++stats.acknowledged;
      o.kind = OutcomeKind::Acknowledged;
      if (answer.ack) {
        o.ack = *answer.ack;
      }
      if (o.ack.client_order_id.empty()) {
        o.ack.client_order_id = p.client_order_id;
      }
    } else {
      ++stats.refused;
      o.kind = OutcomeKind::Refused;
      o.error = RequestError{p.kind, p.client_order_id, answer.error_code, answer.error_msg,
                             static_cast<std::uint64_t>(venue_ms())};
    }
    if (handlers.on_outcome) {
      handlers.on_outcome(o);
    }
  }

  void on_close(std::uint64_t gen, const std::string& reason) {
    const auto it = conns.find(gen);
    if (it == conns.end()) {
      return;
    }
    const State state = it->second.state;
    // The client is inside its own callback: release it once the handler has returned.
    io.post([ws = std::move(it->second.ws)] { static_cast<void>(ws); });
    conns.erase(it);
    fail_pending(gen, reason);
    if (gen == active) {
      active = 0;
      // A connection still draining after a rotation can take over.
      for (auto& [g, c] : conns) {
        if (c.state == State::Draining) {
          c.state = State::Ready;
          active = g;
        }
      }
    }
    if (active == 0 && state != State::Draining) {
      if (announced_ready) {
        announced_ready = false;
        if (handlers.on_down) {
          handlers.on_down(reason);
        }
      }
    }
    if (stopped) {
      return;
    }
    if (active == 0 && !opening()) {
      schedule(reconnect_timer, [](Impl& self) { self.open_connection(); });
    } else if (active != 0 && (state == State::Connecting || state == State::LoggingOn)) {
      schedule(rotate_timer, [](Impl& self) { self.rotate(); }); // the rotation failed; retry
    }
  }

  void schedule(network::Timer& timer, void (*fn)(Impl&)) {
    const std::weak_ptr<Impl> weak = weak_from_this();
    timer.after(backoff.next(), [weak, fn] {
      if (const auto self = weak.lock()) {
        fn(*self);
      }
    });
  }

  void fail_pending(std::uint64_t gen, const std::string& reason) {
    std::vector<Pending> failed;
    for (auto p = pending.begin(); p != pending.end();) {
      if (p->second.gen == gen) {
        failed.push_back(std::move(p->second));
        p = pending.erase(p);
      } else {
        ++p;
      }
    }
    for (const Pending& p : failed) {
      if (p.timed_out || p.logon) {
        continue; // already reported, or nothing to report
      }
      fail(p, reason);
      if (p.order) {
        ++stats.lost;
      }
    }
  }

  void fail(const Pending& p, const std::string& reason) const {
    if (p.order) {
      OrderOutcome o;
      o.kind = OutcomeKind::Unknown;
      o.request = p.kind;
      o.client_order_id = p.client_order_id;
      o.reason = reason;
      o.recv_ns = network::steady_ns();
      if (handlers.on_outcome) {
        handlers.on_outcome(o);
      }
    } else if (p.callback) {
      p.callback(Status::IoError, WsAnswer{}, reason);
    }
  }

  [[nodiscard]] std::int64_t timeout_ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(config.request_timeout).count();
  }

  void arm_timeout() {
    if (timeout_timer.pending()) {
      return;
    }
    std::int64_t earliest = INT64_MAX;
    for (const auto& [id, p] : pending) {
      if (!p.timed_out) {
        earliest = std::min(earliest, p.deadline_ns);
      }
    }
    if (earliest == INT64_MAX) {
      return;
    }
    const std::int64_t delay = std::max<std::int64_t>(earliest - network::steady_ns(), 1'000'000);
    const std::weak_ptr<Impl> weak = weak_from_this();
    timeout_timer.after(std::chrono::nanoseconds{delay}, [weak] {
      if (const auto self = weak.lock()) {
        self->check_timeouts();
      }
    });
  }

  void check_timeouts() {
    const std::int64_t now = network::steady_ns();
    std::vector<Pending> expired;
    std::vector<std::uint64_t> stuck_logons;
    for (auto it = pending.begin(); it != pending.end();) {
      Pending& p = it->second;
      if (p.timed_out && now - p.deadline_ns > 5 * timeout_ns()) {
        it = pending.erase(it); // far too late to matter
        continue;
      }
      if (!p.timed_out && p.deadline_ns <= now) {
        p.timed_out = true;
        ++stats.timeouts;
        if (p.logon) {
          stuck_logons.push_back(p.gen);
        } else {
          expired.push_back(p);
        }
      }
      ++it;
    }
    for (const Pending& p : expired) {
      fail(p, "timeout");
    }
    for (const std::uint64_t gen : stuck_logons) {
      if (const auto c = conns.find(gen); c != conns.end()) {
        c->second.ws->close(1000, "logon timeout"); // on_close reconnects
      }
    }
    arm_timeout();
  }

  std::string next_id() { return "j" + std::to_string(++seq); }

  Status send(std::string_view method, Params params, Security security, Pending p,
              std::string& error, std::string* sent = nullptr) {
    const auto it = conns.find(active);
    if (active == 0 || it == conns.end() || it->second.state != State::Ready) {
      error = "the WebSocket API is not connected";
      return Status::InvalidState;
    }
    const std::string id = next_id();
    std::string message;
    switch (security) {
    case Security::None:
      message = ws_message(id, method, params);
      break;
    case Security::ApiKey:
      params.emplace_back("apiKey", config.api_key);
      message = ws_message(id, method, params);
      break;
    case Security::Signed: {
      params.emplace_back("recvWindow", std::to_string(config.recv_window_ms));
      const bool logged_on = config.signer.ed25519();
      message = ws_request(id, method, std::move(params), venue_ms(),
                           logged_on ? nullptr : &config.signer, config.api_key);
      break;
    }
    }
    p.gen = active;
    p.deadline_ns = network::steady_ns() + timeout_ns();
    pending.emplace(id, std::move(p));
    ++stats.sent;
    if (sent != nullptr) {
      *sent = message;
    }
    it->second.ws->send_text(std::move(message));
    arm_timeout();
    return Status::Ok;
  }

  Status send_order(std::string_view method, RequestKind kind, std::string_view client_order_id,
                    Params params, std::string& error) {
    Pending p;
    p.order = true;
    p.kind = kind;
    p.client_order_id.assign(client_order_id);
    return send(method, std::move(params), Security::Signed, std::move(p), error);
  }

  void shut_down() {
    stopped = true;
    timeout_timer.cancel();
    reconnect_timer.cancel();
    rotate_timer.cancel();
    for (auto& [gen, c] : conns) {
      c.ws->close(1000, "stopped");
    }
  }
};

WsApiSession::WsApiSession(network::IoContext& io, WsApiConfig config, WsApiHandlers handlers)
    : impl_{std::make_shared<Impl>(io, std::move(config), std::move(handlers))} {}

WsApiSession::~WsApiSession() { impl_->shut_down(); }

void WsApiSession::start() {
  if (!impl_->stopped) {
    return;
  }
  impl_->stopped = false;
  impl_->open_connection();
}

void WsApiSession::stop() { impl_->shut_down(); }

bool WsApiSession::ready() const noexcept { return impl_->active != 0; }

WsApiStats WsApiSession::stats() const noexcept { return impl_->stats; }

void WsApiSession::set_time_offset(std::int64_t ms) noexcept { impl_->offset_ms.store(ms); }

Status WsApiSession::place(const model::SubmitOrder& c, std::string_view symbol,
                           std::string& error) {
  Params params;
  if (const Status s = place_params(c, symbol, params, error); !core::ok(s)) {
    return s;
  }
  return impl_->send_order("order.place", RequestKind::Place, c.client_order_id.view(),
                           std::move(params), error);
}

Status WsApiSession::modify(const model::ModifyOrder& c, std::string_view symbol,
                            model::OrderSide side, std::string& error) {
  Params params;
  if (const Status s = modify_params(c, symbol, side, params, error); !core::ok(s)) {
    return s;
  }
  return impl_->send_order("order.modify", RequestKind::Modify, c.client_order_id.view(),
                           std::move(params), error);
}

Status WsApiSession::cancel(const model::CancelOrder& c, std::string_view symbol,
                            std::string& error) {
  Params params;
  if (const Status s = cancel_params(c, symbol, params); !core::ok(s)) {
    error = "the cancel cannot be expressed";
    return s;
  }
  return impl_->send_order("order.cancel", RequestKind::Cancel, c.client_order_id.view(),
                           std::move(params), error);
}

Status WsApiSession::request(std::string_view method, Params params, Security security,
                             Callback callback, std::string& error, std::string* sent) {
  Impl::Pending p;
  p.callback = std::move(callback);
  return impl_->send(method, std::move(params), security, std::move(p), error, sent);
}

} // namespace jarvis::adapter::binance
