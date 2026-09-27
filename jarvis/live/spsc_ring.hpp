#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

// Single-producer single-consumer rings between the live threads (docs/architecture.md section
// 7.1). The core thread is the only writer of engine state; IO threads hand it events and it
// hands commands out only through these rings. Neither side blocks or allocates: a full ring is
// the caller's to handle (drop and count, or back-pressure).
//
// Indices only grow; the slot is the index modulo the power-of-two capacity. Each side keeps a
// cached copy of the other side's index and reads the shared one only when the cache says the
// ring is full (producer) or empty (consumer), so in steady state a push or pop touches one
// shared cache line.

namespace jarvis::live {

inline constexpr std::size_t kCacheLine = 64;

// A ring of fixed-size values.
// The padding between the producer's and the consumer's fields is the point: each side writes
// only its own cache line.
template <typename T>
  requires std::is_trivially_copyable_v<T>
class SpscRing { // NOLINT(clang-analyzer-optin.performance.Padding)
public:
  // The capacity is rounded up to a power of two.
  explicit SpscRing(std::size_t capacity)
      : mask_{std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity) - 1},
        slots_{std::make_unique<T[]>(mask_ + 1)} {} // NOLINT(*-avoid-c-arrays)

  // Producer. False when full.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - cached_tail_ > mask_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - cached_tail_ > mask_) {
        return false;
      }
    }
    slots_[head & mask_] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer. The oldest value, or nullptr when empty; valid until pop().
  [[nodiscard]] const T* front() noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) {
        return nullptr;
      }
    }
    return &slots_[tail & mask_];
  }
  void pop() noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const T* v = front();
    if (v == nullptr) {
      return false;
    }
    out = *v;
    pop();
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }
  // Either side; exact only when the other side is idle.
  [[nodiscard]] std::size_t size() const noexcept {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                    tail_.load(std::memory_order_acquire));
  }

private:
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_; // NOLINT(*-avoid-c-arrays)
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_ = 0; // the producer's
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_ = 0; // the consumer's
};

// A ring of variable-length records, each contiguous in memory: the IO threads write encoded
// events (an order book snapshot can be tens of kilobytes) and the core reads them in place.
// A record is an 8-byte header (u32 length, u32 kind) and its bytes, padded to 8. One that does
// not fit before the end of the buffer is preceded by a wrap mark and written at the start.
class SpscByteRing { // NOLINT(clang-analyzer-optin.performance.Padding): as SpscRing
public:
  // The capacity in bytes is rounded up to a power of two, at least 64.
  explicit SpscByteRing(std::size_t capacity)
      : mask_{std::bit_ceil(capacity < 64 ? std::size_t{64} : capacity) - 1},
        data_{std::make_unique<std::byte[]>(mask_ + 1)} {} // NOLINT(*-avoid-c-arrays)

  // The largest record: half the ring, so that a wrap never needs more than the whole buffer.
  [[nodiscard]] std::size_t max_record() const noexcept { return (mask_ + 1) / 2 - kHeader; }

  // Producer: room for `n` bytes, or an empty span when the ring is full or `n` is too large.
  // Nothing is visible to the consumer until commit(); a later reserve() discards this one.
  [[nodiscard]] std::span<std::byte> reserve(std::size_t n) noexcept {
    if (n > max_record()) {
      return {};
    }
    std::uint64_t pos = head_.load(std::memory_order_relaxed);
    const std::size_t total = padded(n);
    const std::size_t to_end = mask_ + 1 - (pos & mask_);
    const std::size_t pad = to_end < total ? to_end : 0;
    if (pos + pad + total - cached_tail_ > mask_ + 1) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (pos + pad + total - cached_tail_ > mask_ + 1) {
        return {};
      }
    }
    if (pad > 0) {
      write_header(pos, 0, kWrap);
      pos += pad;
    }
    reserved_ = pos;
    return {data_.get() + (pos & mask_) + kHeader, n};
  }
  // Publishes the reserved record with its first `n` bytes (n no more than reserved).
  void commit(std::size_t n) noexcept {
    write_header(reserved_, static_cast<std::uint32_t>(n), kRecord);
    head_.store(reserved_ + padded(n), std::memory_order_release);
  }
  [[nodiscard]] bool try_write(std::span<const std::byte> bytes) noexcept {
    const std::span<std::byte> room = reserve(bytes.size());
    if (room.data() == nullptr) {
      return false;
    }
    if (!bytes.empty()) {
      std::memcpy(room.data(), bytes.data(), bytes.size());
    }
    commit(bytes.size());
    return true;
  }

  // Consumer: the oldest record in place, or an empty span with `empty` set. Valid until
  // release().
  [[nodiscard]] std::span<const std::byte> peek(bool& empty) noexcept {
    std::uint64_t pos = tail_.load(std::memory_order_relaxed);
    if (!available(pos)) {
      empty = true;
      return {};
    }
    std::uint32_t length = 0;
    std::uint32_t kind = 0;
    read_header(pos, length, kind);
    if (kind == kWrap) {
      pos += mask_ + 1 - (pos & mask_); // the record follows at the start
      tail_.store(pos, std::memory_order_release);
      read_header(pos, length, kind);
    }
    empty = false;
    current_ = pos;
    current_size_ = padded(length);
    return {data_.get() + (pos & mask_) + kHeader, length};
  }
  void release() noexcept { tail_.store(current_ + current_size_, std::memory_order_release); }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }
  [[nodiscard]] std::size_t used() const noexcept {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                    tail_.load(std::memory_order_acquire));
  }

private:
  static constexpr std::size_t kHeader = 8;
  static constexpr std::uint32_t kRecord = 1;
  static constexpr std::uint32_t kWrap = 2;

  [[nodiscard]] static constexpr std::size_t padded(std::size_t n) noexcept {
    return (kHeader + n + 7U) & ~std::size_t{7};
  }

  bool available(std::uint64_t pos) noexcept {
    if (pos != cached_head_) {
      return true;
    }
    cached_head_ = head_.load(std::memory_order_acquire);
    return pos != cached_head_;
  }

  void write_header(std::uint64_t pos, std::uint32_t length, std::uint32_t kind) noexcept {
    std::byte* p = data_.get() + (pos & mask_);
    std::memcpy(p, &length, sizeof length);
    std::memcpy(p + 4, &kind, sizeof kind);
  }
  void read_header(std::uint64_t pos, std::uint32_t& length, std::uint32_t& kind) const noexcept {
    const std::byte* p = data_.get() + (pos & mask_);
    std::memcpy(&length, p, sizeof length);
    std::memcpy(&kind, p + 4, sizeof kind);
  }

  const std::size_t mask_;
  std::unique_ptr<std::byte[]> data_; // NOLINT(*-avoid-c-arrays)
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_ = 0; // the producer's
  std::uint64_t reserved_ = 0;
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_ = 0; // the consumer's
  std::uint64_t current_ = 0;
  std::size_t current_size_ = 0;
};

} // namespace jarvis::live
