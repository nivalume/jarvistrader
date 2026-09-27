#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "jarvis/backtest/matching/sim_exchange.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/cost/latency.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"

// The backtest's two timelines (docs/architecture.md section 12.2). The simulated venue acts at
// venue time; the kernel observes later:
//
//   market data   the venue matches on it at its recorded ts; the kernel sees it
//                 L_feed later (its ts_init is rewritten to that time);
//   commands      a command the kernel emits at t reaches the venue at t + L_out;
//   answers       an order event the venue emits at t reaches the kernel at t + L_in.
//
// Each direction is one ordered channel (the feed, the WS API connection, the user data stream),
// so every channel is a FIFO: a delay never lets a later message overtake an earlier one on the
// same channel. Delays come from JitteredLatency, keyed by the data counter, the ClientOrderId and
// the answer counter: a function of the inputs, like everything else.
//
// The driver asks for the next venue-side time and the next kernel input time and processes the
// earliest (venue side first on a tie); see Driver in venue mode.

namespace jarvis::backtest {

// source_id of the venue's answers in the run log.
inline constexpr std::uint16_t kVenueSource = 0xFFFE;

// What the kernel needs of a venue (docs/architecture.md section 12.1): it takes commands and
// answers with order events. The simulated exchange satisfies it; the live adapter (M4) will
// satisfy the same contract behind its connections.
template <typename V>
concept VenueClient = requires(V& v, const model::Output& command, core::UnixNanos now) {
  { v.on_command(command, now) } -> std::same_as<core::Status>;
  { v.events() } -> std::same_as<std::span<const model::OrderEvent>>;
  v.clear_events();
};

static_assert(VenueClient<SimulatedExchange>);

struct VenueLoopConfig {
  SimConfig sim;
  std::uint64_t feed_ns = 0;
  std::uint64_t out_ns = 0;
  std::uint64_t in_ns = 0;
  std::uint64_t jitter_ns = 0;
  std::uint64_t seed = 0;
  std::uint32_t pending = 4096;     // kernel inputs in flight per channel
  std::uint32_t delta_pool = 32768; // book deltas held by delayed market data
  std::uint32_t commands = 4096;    // commands in flight
  std::optional<core::UnixNanos> start;
  std::optional<core::UnixNanos> end;
};

struct VenueLoopStats {
  std::uint64_t data = 0;     // market data events the venue saw
  std::uint64_t answers = 0;  // order events the venue sent
  std::uint64_t commands = 0; // commands delivered to the venue
  std::uint64_t skipped = 0;  // data outside [start, end)
};

namespace detail {

// A fixed-capacity FIFO.
template <typename T> class Ring {
public:
  explicit Ring(std::size_t capacity) : items_{capacity} {
    for (std::size_t i = 0; i < capacity; ++i) {
      static_cast<void>(items_.push_back(T{}));
    }
  }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] const T& front() const noexcept { return items_[head_]; }
  [[nodiscard]] T& front() noexcept { return items_[head_]; }
  [[nodiscard]] core::Status push(const T& item) noexcept {
    if (count_ == items_.size()) {
      return core::Status::CapacityExceeded;
    }
    items_[(head_ + count_) % items_.size()] = item;
    ++count_;
    return core::Status::Ok;
  }
  void pop() noexcept {
    head_ = (head_ + 1) % items_.size();
    --count_;
  }

private:
  core::FixedVector<T> items_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

// Contiguous storage for the deltas of delayed OrderBookDeltas, allocated and released in FIFO
// order.
class DeltaPool {
public:
  explicit DeltaPool(std::size_t capacity) : items_{capacity} {
    for (std::size_t i = 0; i < capacity; ++i) {
      static_cast<void>(items_.push_back(model::OrderBookDelta{}));
    }
  }

  [[nodiscard]] core::Status alloc(std::size_t n, std::size_t& offset) noexcept {
    if (live_ == 0) {
      head_ = tail_ = 0;
      wrapped_ = false;
    }
    const std::size_t cap = items_.size();
    if (!wrapped_) {
      if (tail_ + n <= cap) {
        offset = tail_;
      } else if (n <= head_) {
        offset = 0;
        wrapped_ = true;
      } else {
        return core::Status::CapacityExceeded;
      }
    } else if (tail_ + n <= head_) {
      offset = tail_;
    } else {
      return core::Status::CapacityExceeded;
    }
    tail_ = offset + n;
    ++live_;
    return core::Status::Ok;
  }

  void release(std::size_t offset, std::size_t n) noexcept {
    if (offset < head_) {
      wrapped_ = false; // the oldest live allocation is the first one after the wrap
    }
    head_ = offset + n;
    --live_;
  }

  [[nodiscard]] std::span<model::OrderBookDelta> at(std::size_t offset, std::size_t n) noexcept {
    return items_.span().subspan(offset, n);
  }

private:
  core::FixedVector<model::OrderBookDelta> items_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t live_ = 0;
  bool wrapped_ = false;
};

[[nodiscard]] inline std::uint64_t fnv1a(std::string_view text) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const char c : text) {
    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ULL;
  }
  return h;
}

// Sets an event's ts_init (the time the kernel observes it). Not noexcept: std::visit may throw
// bad_variant_access, which cannot happen here.
inline void set_ts_init(model::Event& event, core::UnixNanos ts) {
  std::visit(
      [ts](auto& e) {
        if constexpr (requires { e.ts_init; }) {
          e.ts_init = ts;
        } else if constexpr (requires { e.header.ts_init; }) {
          e.header.ts_init = ts;
        } else if constexpr (requires { e.common.ts_init; }) {
          e.common.ts_init = ts;
        }
      },
      event);
}

} // namespace detail

template <engine::EventSource Source> class VenueLoop {
public:
  VenueLoop(const VenueLoopConfig& c, Source& source)
      : config_{c}, source_{&source}, exchange_{c.sim},
        latency_{c.seed, c.feed_ns, c.out_ns, c.in_ns, c.jitter_ns}, data_{c.pending},
        answers_{c.pending}, commands_{c.commands}, pool_{c.delta_pool} {}

  VenueLoop(const VenueLoop&) = delete;
  VenueLoop& operator=(const VenueLoop&) = delete;
  VenueLoop(VenueLoop&&) = delete;
  VenueLoop& operator=(VenueLoop&&) = delete;
  ~VenueLoop() = default;

  [[nodiscard]] SimulatedExchange& exchange() noexcept { return exchange_; }
  [[nodiscard]] const VenueLoopStats& stats() const noexcept { return stats_; }

  // The earliest venue-side time: the next market data event or command arrival.
  [[nodiscard]] core::Status next_venue_time(std::optional<core::UnixNanos>& out) {
    out.reset();
    const core::Status s = fill_lookahead();
    if (!core::ok(s)) {
      return s;
    }
    if (lookahead_) {
      out = lookahead_key_.ts;
    }
    if (!commands_.empty() && (!out || commands_.front().at < *out)) {
      out = commands_.front().at;
    }
    return core::Status::Ok;
  }

  // The earliest kernel input: answers before market data at the same time (the user data
  // stream drains first, section 5.5).
  [[nodiscard]] std::optional<core::UnixNanos> next_input_time() const noexcept {
    std::optional<core::UnixNanos> t;
    if (!answers_.empty()) {
      t = answers_.front().key.ts;
    }
    if (!data_.empty() && (!t || data_.front().key.ts < *t)) {
      t = data_.front().key.ts;
    }
    return t;
  }

  // Processes the earliest venue-side item (market data first on a tie).
  [[nodiscard]] core::Status process_venue() {
    core::Status s = fill_lookahead();
    if (!core::ok(s)) {
      return s;
    }
    const bool take_data =
        lookahead_ && (commands_.empty() || !(commands_.front().at < lookahead_key_.ts));
    if (take_data) {
      return process_data();
    }
    if (commands_.empty()) {
      return core::Status::Ok;
    }
    const Command c = commands_.front();
    commands_.pop();
    ++stats_.commands;
    exchange_.clear_events();
    s = exchange_.on_command(c.output, c.at);
    if (!core::ok(s)) {
      return s;
    }
    return schedule_answers(c.at);
  }

  // Pops the earliest kernel input; false when none is pending.
  [[nodiscard]] bool pop_input(core::EventKey& key, model::Event& event) {
    const bool answer =
        !answers_.empty() && (data_.empty() || !(data_.front().key.ts < answers_.front().key.ts));
    if (answer) {
      key = answers_.front().key;
      event = answers_.front().event;
      answers_.pop();
      return true;
    }
    if (data_.empty()) {
      return false;
    }
    Delayed& d = data_.front();
    key = d.key;
    event = d.event;
    // The previous delayed deltas have been stepped by now; this event's stay valid until the
    // next pop (the driver steps each input before asking for the next).
    if (has_pending_release_) {
      pool_.release(pending_release_offset_, pending_release_n_);
      has_pending_release_ = false;
    }
    if (d.deltas > 0) {
      pending_release_offset_ = d.offset;
      pending_release_n_ = d.deltas;
      has_pending_release_ = true;
    }
    data_.pop();
    return true;
  }

  // Schedules the commands a step at kernel time `now` emitted.
  [[nodiscard]] core::Status on_outputs(core::UnixNanos now, std::span<const model::Output> out) {
    for (const model::Output& o : out) {
      const model::ClientOrderId* cid = client_order_id(o);
      if (cid == nullptr && !std::holds_alternative<model::CancelAllOrders>(o)) {
        continue; // features, records, denials: not for the venue
      }
      const std::uint64_t identity = cid != nullptr ? detail::fnv1a(cid->view()) : 0;
      const core::UnixNanos at = fifo(
          last_command_,
          core::UnixNanos{now.value() + latency_
                                            .delay(identity, cost::LatencyHop::Outbound,
                                                   static_cast<std::uint32_t>(++command_serial_))
                                            .value()});
      const core::Status s = commands_.push(Command{at, o});
      if (!core::ok(s)) {
        return s;
      }
    }
    return core::Status::Ok;
  }

  [[nodiscard]] bool exhausted() const noexcept {
    return source_done_ && !lookahead_ && commands_.empty() && data_.empty() && answers_.empty();
  }

private:
  struct Delayed {
    core::EventKey key;
    model::Event event;
    std::size_t offset = 0; // delta pool storage
    std::size_t deltas = 0;
  };
  struct Command {
    core::UnixNanos at;
    model::Output output;
  };

  static core::UnixNanos fifo(core::UnixNanos& last, core::UnixNanos t) noexcept {
    if (t < last) {
      t = last;
    }
    last = t;
    return t;
  }

  [[nodiscard]] static const model::ClientOrderId*
  client_order_id(const model::Output& o) noexcept {
    if (const auto* s = std::get_if<model::SubmitOrder>(&o)) {
      return &s->client_order_id;
    }
    if (const auto* m = std::get_if<model::ModifyOrder>(&o)) {
      return &m->client_order_id;
    }
    if (const auto* c = std::get_if<model::CancelOrder>(&o)) {
      return &c->client_order_id;
    }
    return nullptr;
  }

  core::Status fill_lookahead() {
    while (!lookahead_ && !source_done_) {
      const core::Status s = source_->next(lookahead_key_, lookahead_event_);
      if (s == core::Status::EndOfStream) {
        source_done_ = true;
        return core::Status::Ok;
      }
      if (!core::ok(s)) {
        return s;
      }
      if (config_.end && !(lookahead_key_.ts < *config_.end)) {
        ++stats_.skipped;
        source_done_ = true;
        return core::Status::Ok;
      }
      if (config_.start && lookahead_key_.ts < *config_.start) {
        ++stats_.skipped;
        continue;
      }
      lookahead_ = true;
    }
    return core::Status::Ok;
  }

  core::Status process_data() {
    const core::UnixNanos venue_time = lookahead_key_.ts;
    ++stats_.data;
    exchange_.clear_events();
    core::Status s = exchange_.on_data(lookahead_event_, venue_time);
    if (!core::ok(s)) {
      return s;
    }
    s = schedule_answers(venue_time);
    if (!core::ok(s)) {
      return s;
    }
    Delayed d;
    d.key = lookahead_key_;
    d.key.ts =
        fifo(last_data_,
             core::UnixNanos{venue_time.value() +
                             latency_.delay(++data_serial_, cost::LatencyHop::Feed).value()});
    d.event = lookahead_event_;
    detail::set_ts_init(d.event, d.key.ts);
    if (auto* deltas = std::get_if<model::OrderBookDeltas>(&d.event)) {
      const std::size_t n = deltas->deltas.size();
      s = pool_.alloc(n, d.offset);
      if (!core::ok(s)) {
        return s;
      }
      const std::span<model::OrderBookDelta> copy = pool_.at(d.offset, n);
      for (std::size_t i = 0; i < n; ++i) {
        copy[i] = deltas->deltas[i];
        copy[i].ts_init = d.key.ts;
      }
      deltas->deltas = copy;
      d.deltas = n;
    }
    lookahead_ = false; // the source may now move on
    return data_.push(d);
  }

  core::Status schedule_answers(core::UnixNanos venue_time) {
    for (const model::OrderEvent& e : exchange_.events()) {
      const core::UnixNanos at = fifo(
          last_answer_,
          core::UnixNanos{venue_time.value() +
                          latency_.delay(++answer_serial_, cost::LatencyHop::Inbound).value()});
      model::Event event = std::visit([](const auto& x) { return model::Event{x}; }, e);
      detail::set_ts_init(event, at);
      const core::Status s =
          answers_.push(Delayed{core::EventKey{at, kVenueSource, 0}, event, 0, 0});
      if (!core::ok(s)) {
        return s;
      }
      ++stats_.answers;
    }
    exchange_.clear_events();
    return core::Status::Ok;
  }

  VenueLoopConfig config_;
  Source* source_;
  SimulatedExchange exchange_;
  cost::JitteredLatency latency_;
  detail::Ring<Delayed> data_;
  detail::Ring<Delayed> answers_;
  detail::Ring<Command> commands_;
  detail::DeltaPool pool_;
  VenueLoopStats stats_;
  core::EventKey lookahead_key_;
  model::Event lookahead_event_;
  bool lookahead_ = false;
  bool source_done_ = false;
  core::UnixNanos last_data_;
  core::UnixNanos last_answer_;
  core::UnixNanos last_command_;
  std::uint64_t data_serial_ = 0;
  std::uint64_t answer_serial_ = 0;
  std::uint64_t command_serial_ = 0;
  std::size_t pending_release_offset_ = 0;
  std::size_t pending_release_n_ = 0;
  bool has_pending_release_ = false;
};

} // namespace jarvis::backtest
