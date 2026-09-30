#pragma once

#include <cstddef>
#include <cstdint>

#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/identifiers.hpp"

namespace jarvis::model {

// Dense 32-bit handle for an interned InstrumentId (docs/architecture.md section 6.2). Kernel
// state that is kept per instrument (books, subscriptions, positions, risk limits) is indexed by
// slot, so the hot path never hashes or compares instrument strings.
struct InstrumentSlot {
  std::uint32_t value = 0;

  template <typename Ar> void state(Ar& ar) { ar(value); }

  friend constexpr bool operator==(InstrumentSlot, InstrumentSlot) noexcept = default;
};

// Side table from InstrumentId to InstrumentSlot. Slots are handed out in first-seen order, which
// is deterministic because event order is. Storage is sized once at construction (wire time);
// interning never allocates. Open addressing with linear probing over a power-of-two bucket
// array at most half full.
class InstrumentTable {
public:
  explicit InstrumentTable(std::uint32_t capacity)
      : ids_{capacity}, buckets_{bucket_count(capacity)}, mask_{bucket_count(capacity) - 1} {
    for (std::size_t i = 0; i < buckets_.capacity(); ++i) {
      static_cast<void>(buckets_.push_back(kEmpty));
    }
  }

  // The slot of `id`, adding it when new. CapacityExceeded when the table is full.
  [[nodiscard]] core::Status intern(const InstrumentId& id, InstrumentSlot& out) noexcept {
    std::size_t bucket = hash(id) & mask_;
    while (buckets_[bucket] != kEmpty) {
      const std::uint32_t slot = buckets_[bucket] - 1;
      if (ids_[slot] == id) {
        out = InstrumentSlot{slot};
        return core::Status::Ok;
      }
      bucket = (bucket + 1) & mask_;
    }
    const auto slot = static_cast<std::uint32_t>(ids_.size());
    const core::Status s = ids_.push_back(id);
    if (!core::ok(s)) {
      return s;
    }
    buckets_[bucket] = slot + 1;
    out = InstrumentSlot{slot};
    return core::Status::Ok;
  }

  // The slot of `id`, or NotFound.
  [[nodiscard]] core::Status find(const InstrumentId& id, InstrumentSlot& out) const noexcept {
    std::size_t bucket = hash(id) & mask_;
    while (buckets_[bucket] != kEmpty) {
      const std::uint32_t slot = buckets_[bucket] - 1;
      if (ids_[slot] == id) {
        out = InstrumentSlot{slot};
        return core::Status::Ok;
      }
      bucket = (bucket + 1) & mask_;
    }
    return core::Status::NotFound;
  }

  // The id behind `slot`; `slot` must have come from this table.
  [[nodiscard]] const InstrumentId& id(InstrumentSlot slot) const noexcept {
    return ids_[slot.value];
  }

  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(ids_.size());
  }
  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return static_cast<std::uint32_t>(ids_.capacity());
  }

private:
  static constexpr std::uint32_t kEmpty = 0; // buckets hold slot + 1

  [[nodiscard]] static constexpr std::size_t bucket_count(std::uint32_t capacity) noexcept {
    std::size_t n = 2;
    while (n < 2 * static_cast<std::size_t>(capacity)) {
      n *= 2;
    }
    return n;
  }

  // FNV-1a over "symbol.venue"; stable across platforms and runs.
  [[nodiscard]] static constexpr std::size_t hash(const InstrumentId& id) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](char c) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 0x100000001b3ULL;
    };
    for (const char c : id.symbol.view()) {
      mix(c);
    }
    mix('.');
    for (const char c : id.venue.view()) {
      mix(c);
    }
    return static_cast<std::size_t>(h);
  }

  core::FixedVector<InstrumentId> ids_;
  core::FixedVector<std::uint32_t> buckets_;
  std::size_t mask_;

public:
  // Snapshot encoding (core/state.hpp); the bucket array follows the capacity.
  template <typename Ar> void state(Ar& ar) { ar(ids_, buckets_); }
};

} // namespace jarvis::model
