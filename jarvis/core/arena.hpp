#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "jarvis/core/status.hpp"

namespace jarvis::core {

// Index into an Arena plus the generation of the slot at the time the handle was issued. A handle
// to an erased element never aliases the element that later reuses the slot (no ABA).
template <typename Tag> struct Handle {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  friend constexpr bool operator==(Handle, Handle) noexcept = default;
  friend constexpr auto operator<=>(Handle, Handle) noexcept = default;
};

// Fixed-capacity slab of T addressed by generation-tagged handles. Capacity is fixed at
// construction; insert returns Status::CapacityExceeded when full. Slot reuse is LIFO, so the
// same sequence of operations always yields the same handles.
template <typename T, typename Tag = T> class Arena {
public:
  using HandleType = Handle<Tag>;

  explicit Arena(std::uint32_t capacity) : slots_(capacity) {
    free_.reserve(capacity);
    for (std::uint32_t i = capacity; i > 0; --i) {
      free_.push_back(i - 1);
    }
  }

  [[nodiscard]] Status insert(const T& value, HandleType& out) noexcept {
    if (free_.empty()) {
      return Status::CapacityExceeded;
    }
    const std::uint32_t index = free_.back();
    free_.pop_back();
    Slot& slot = slots_[index];
    slot.value = value;
    slot.occupied = true;
    out = HandleType{index, slot.generation};
    ++size_;
    return Status::Ok;
  }

  [[nodiscard]] T* get(HandleType handle) noexcept {
    return valid(handle) ? &slots_[handle.index].value : nullptr;
  }
  [[nodiscard]] const T* get(HandleType handle) const noexcept {
    return valid(handle) ? &slots_[handle.index].value : nullptr;
  }

  [[nodiscard]] Status erase(HandleType handle) noexcept {
    if (!valid(handle)) {
      return Status::NotFound;
    }
    Slot& slot = slots_[handle.index];
    slot.occupied = false;
    ++slot.generation;
    free_.push_back(handle.index);
    --size_;
    return Status::Ok;
  }

  [[nodiscard]] bool valid(HandleType handle) const noexcept {
    return handle.index < slots_.size() && slots_[handle.index].occupied &&
           slots_[handle.index].generation == handle.generation;
  }

  // Visits live elements in slot order, which is deterministic.
  template <typename Visitor> void for_each(Visitor&& visit) const {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].occupied) {
        visit(HandleType{i, slots_[i].generation}, slots_[i].value);
      }
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
  struct Slot {
    T value{};
    std::uint32_t generation = 0;
    bool occupied = false;
  };

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_;
  std::size_t size_ = 0;
};

} // namespace jarvis::core
