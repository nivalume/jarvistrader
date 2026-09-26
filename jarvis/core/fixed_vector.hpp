#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "jarvis/core/status.hpp"

namespace jarvis::core {

// A vector whose capacity is fixed when it is constructed (from configuration, at startup).
// Growth past the capacity returns Status::CapacityExceeded instead of reallocating, so no
// operation after construction allocates.
template <typename T> class FixedVector {
public:
  explicit FixedVector(std::size_t capacity) : capacity_{capacity} { items_.reserve(capacity); }

  // Copies keep the full reservation, so a copy never reallocates later either.
  FixedVector(const FixedVector& other) : capacity_{other.capacity_} {
    items_.reserve(capacity_);
    items_.assign(other.items_.begin(), other.items_.end());
  }
  FixedVector& operator=(const FixedVector& other) {
    if (this != &other) {
      std::vector<T> items;
      items.reserve(other.capacity_);
      items.assign(other.items_.begin(), other.items_.end());
      items_ = std::move(items);
      capacity_ = other.capacity_;
    }
    return *this;
  }
  FixedVector(FixedVector&&) noexcept = default;
  FixedVector& operator=(FixedVector&&) noexcept = default;
  ~FixedVector() = default;

  [[nodiscard]] Status push_back(const T& value) noexcept {
    if (items_.size() >= capacity_) {
      return Status::CapacityExceeded;
    }
    items_.push_back(value);
    return Status::Ok;
  }
  [[nodiscard]] Status push_back(T&& value) noexcept {
    if (items_.size() >= capacity_) {
      return Status::CapacityExceeded;
    }
    items_.push_back(std::move(value));
    return Status::Ok;
  }

  // Removes the element at `index` by moving the last element into its place. O(1); does not
  // preserve order.
  [[nodiscard]] Status swap_remove(std::size_t index) noexcept {
    if (index >= items_.size()) {
      return Status::OutOfRange;
    }
    items_[index] = items_.back();
    items_.pop_back();
    return Status::Ok;
  }

  void pop_back() noexcept {
    if (!items_.empty()) {
      items_.pop_back();
    }
  }
  void clear() noexcept { items_.clear(); }

  [[nodiscard]] T& operator[](std::size_t index) noexcept { return items_[index]; }
  [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return items_[index]; }
  [[nodiscard]] T& back() noexcept { return items_.back(); }
  [[nodiscard]] const T& back() const noexcept { return items_.back(); }

  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] bool full() const noexcept { return items_.size() >= capacity_; }

  [[nodiscard]] std::span<T> span() noexcept { return std::span<T>{items_}; }
  [[nodiscard]] std::span<const T> span() const noexcept { return std::span<const T>{items_}; }

private:
  std::vector<T> items_;
  std::size_t capacity_;
};

} // namespace jarvis::core
