#pragma once

#include <cstddef>
#include <utility>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"

namespace jarvis::core {

// Fixed-capacity min-heap ordered by EventKey. Keys are unique by construction (section 5.2), so
// the pop order is fully determined by the keys and never by heap layout.
template <typename T> class PriorityQueue {
public:
  struct Entry {
    EventKey key;
    T payload;

    template <typename Ar> void state(Ar& ar) { ar(key, payload); }
  };

  explicit PriorityQueue(std::size_t capacity) : heap_{capacity} {}

  [[nodiscard]] Status push(const EventKey& key, const T& payload) noexcept {
    const Status s = heap_.push_back(Entry{key, payload});
    if (!ok(s)) {
      return s;
    }
    sift_up(heap_.size() - 1);
    return Status::Ok;
  }

  // Precondition: not empty.
  [[nodiscard]] const Entry& top() const noexcept { return heap_[0]; }

  [[nodiscard]] bool pop(Entry& out) noexcept {
    if (heap_.empty()) {
      return false;
    }
    out = heap_[0];
    heap_[0] = heap_.back();
    heap_.pop_back();
    if (!heap_.empty()) {
      sift_down(0);
    }
    return true;
  }

  // Keeps only entries for which keep(entry) is true, then restores the heap. Used to purge
  // cancelled entries without allocating.
  template <typename Predicate> void retain(Predicate&& keep) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < heap_.size(); ++read) {
      if (keep(heap_[read])) {
        heap_[write++] = heap_[read];
      }
    }
    while (heap_.size() > write) {
      heap_.pop_back();
    }
    for (std::size_t i = heap_.size() / 2; i > 0; --i) {
      sift_down(i - 1);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
  [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
  [[nodiscard]] bool full() const noexcept { return heap_.full(); }
  void clear() noexcept { heap_.clear(); }

  // Snapshot encoding: the heap array as it is (a valid heap stays one).
  template <typename Ar> void state(Ar& ar) { ar(heap_); }

private:
  void sift_up(std::size_t i) noexcept {
    while (i > 0) {
      const std::size_t parent = (i - 1) / 2;
      if (!(heap_[i].key < heap_[parent].key)) {
        break;
      }
      std::swap(heap_[i], heap_[parent]);
      i = parent;
    }
  }

  void sift_down(std::size_t i) noexcept {
    const std::size_t n = heap_.size();
    while (true) {
      const std::size_t left = 2 * i + 1;
      const std::size_t right = left + 1;
      std::size_t smallest = i;
      if (left < n && heap_[left].key < heap_[smallest].key) {
        smallest = left;
      }
      if (right < n && heap_[right].key < heap_[smallest].key) {
        smallest = right;
      }
      if (smallest == i) {
        return;
      }
      std::swap(heap_[i], heap_[smallest]);
      i = smallest;
    }
  }

  FixedVector<Entry> heap_;
};

} // namespace jarvis::core
