#pragma once

#include <concepts>
#include <cstdint>
#include <optional>

#include "jarvis/core/arena.hpp"
#include "jarvis/core/priority_queue.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"

namespace jarvis::core {

template <typename C>
concept Clock = requires(const C& clock) {
  { clock.now() } -> std::same_as<UnixNanos>;
};

// Clock of a backtest: time moves only when the engine advances it to the next event.
class ReplayClock {
public:
  constexpr ReplayClock() noexcept = default;
  explicit constexpr ReplayClock(UnixNanos start) noexcept : now_{start} {}

  [[nodiscard]] constexpr UnixNanos now() const noexcept { return now_; }

  // Time never goes backwards.
  [[nodiscard]] constexpr Status advance_to(UnixNanos t) noexcept {
    if (t < now_) {
      return Status::InvalidArgument;
    }
    now_ = t;
    return Status::Ok;
  }

private:
  UnixNanos now_;
};

static_assert(Clock<ReplayClock>);

// Caller-chosen identity of a timer: the owner (a strategy index, an algorithm) and an id.
struct TimerKey {
  std::uint32_t owner = 0;
  std::uint32_t id = 0;

  template <typename Ar> void state(Ar& ar) { ar(owner, id); }

  friend constexpr bool operator==(TimerKey, TimerKey) noexcept = default;
};

struct TimerTag;
using TimerHandle = Handle<TimerTag>;

struct FiredTimer {
  TimerHandle handle;
  TimerKey key;
  UnixNanos deadline;
};

// Deterministic timer queue. Timers fire in (deadline, schedule order); a periodic timer is
// re-armed at deadline + period under the same handle. Cancelled timers leave stale heap entries
// that are skipped on pop and purged in place when the heap fills, so nothing allocates after
// construction.
class TimerQueue {
public:
  explicit TimerQueue(std::uint32_t capacity)
      : slots_{capacity}, heap_{std::size_t{capacity} * 2} {}

  [[nodiscard]] Status schedule(UnixNanos deadline, DurationNanos period, TimerKey key,
                                TimerHandle& out) noexcept {
    if (heap_.full()) {
      purge();
    }
    TimerHandle handle;
    const Status s = slots_.insert(Slot{key, period, 0}, handle);
    if (!ok(s)) {
      return s;
    }
    const Status pushed = arm(handle, deadline);
    if (!ok(pushed)) {
      static_cast<void>(slots_.erase(handle));
      return pushed;
    }
    out = handle;
    return Status::Ok;
  }

  [[nodiscard]] Status cancel(TimerHandle handle) noexcept { return slots_.erase(handle); }

  // Pops the next live timer whose deadline is at or before `now`.
  [[nodiscard]] bool pop_due(UnixNanos now, FiredTimer& out) noexcept {
    while (!heap_.empty() && heap_.top().key.ts <= now) {
      PriorityQueue<Entry>::Entry entry;
      static_cast<void>(heap_.pop(entry));
      Slot* slot = slots_.get(entry.payload.handle);
      if (slot == nullptr || slot->armed_seq != entry.key.seq) {
        continue; // cancelled, or superseded by a re-arm
      }
      out = FiredTimer{entry.payload.handle, slot->key, entry.key.ts};
      if (slot->period.value() == 0) {
        static_cast<void>(slots_.erase(entry.payload.handle));
      } else {
        UnixNanos next;
        if (!ok(entry.key.ts.plus(slot->period, next)) || !ok(arm(entry.payload.handle, next))) {
          static_cast<void>(slots_.erase(entry.payload.handle));
        }
      }
      return true;
    }
    return false;
  }

  // Pops the stale entries off the top of the heap, so that the top is a live timer. The engine
  // calls it at the end of every step: between steps peek() then finds the next timer at once.
  void prune() noexcept {
    while (!heap_.empty()) {
      const auto& top = heap_.top();
      const Slot* slot = slots_.get(top.payload.handle);
      if (slot != nullptr && slot->armed_seq == top.key.seq) {
        return;
      }
      PriorityQueue<Entry>::Entry discarded;
      static_cast<void>(heap_.pop(discarded));
    }
  }

  // The next live timer (earliest deadline, then schedule order) without firing it. It changes
  // nothing, not even the stale entries: what a node's driver asks between steps must leave the
  // state as a replay, which does not ask, has it (EngineState snapshots compare it). After
  // prune() the answer is the top entry.
  [[nodiscard]] bool peek(FiredTimer& out) const noexcept {
    const auto* next = heap_.min_where([this](const PriorityQueue<Entry>::Entry& e) {
      const Slot* slot = slots_.get(e.payload.handle);
      return slot != nullptr && slot->armed_seq == e.key.seq;
    });
    if (next == nullptr) {
      return false;
    }
    out = FiredTimer{next->payload.handle, slots_.get(next->payload.handle)->key, next->key.ts};
    return true;
  }

  [[nodiscard]] std::optional<UnixNanos> next_deadline() const noexcept {
    FiredTimer next;
    if (!peek(next)) {
      return std::nullopt;
    }
    return next.deadline;
  }

  [[nodiscard]] std::size_t active() const noexcept { return slots_.size(); }

  // Snapshot encoding: the timers, the heap with its stale entries, and the arming counter.
  template <typename Ar> void state(Ar& ar) { ar(slots_, heap_, seq_); }

private:
  struct Slot {
    TimerKey key;
    DurationNanos period;
    std::uint64_t armed_seq = 0;

    template <typename Ar> void state(Ar& ar) { ar(key, period, armed_seq); }
  };
  struct Entry {
    TimerHandle handle;

    template <typename Ar> void state(Ar& ar) { ar(handle); }
  };

  [[nodiscard]] Status arm(TimerHandle handle, UnixNanos deadline) noexcept {
    if (heap_.full()) {
      purge();
    }
    Slot* slot = slots_.get(handle);
    slot->armed_seq = ++seq_;
    return heap_.push(EventKey{deadline, 0, seq_}, Entry{handle});
  }

  void purge() {
    heap_.retain([this](const PriorityQueue<Entry>::Entry& e) {
      const Slot* slot = slots_.get(e.payload.handle);
      return slot != nullptr && slot->armed_seq == e.key.seq;
    });
  }

  Arena<Slot, TimerTag> slots_;
  PriorityQueue<Entry> heap_;
  std::uint64_t seq_ = 0;
};

} // namespace jarvis::core
