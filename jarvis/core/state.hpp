#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"

// The explicit encoding of kernel state for EngineState snapshots (docs/architecture.md section
// 16.3). Like the event log it never writes struct padding, pointers or container capacity the
// configuration already fixes: every value is written field by field, little-endian, at a fixed
// width.
//
// A stateful class describes itself once, for both directions:
//
//   template <typename Ar> void state(Ar& ar) { ar(a_, b_, c_); }
//
// StateWriter reads the fields (it never modifies them), StateReader assigns them. `ar(x)` picks
// the encoding by type: a member state(ar) first, then bool, enums (their underlying width),
// integers, 128-bit integers, and then a state_io(ar, x) overload: the ones below for the core
// and standard types, and those found by argument-dependent lookup (model/state_io.hpp for the
// model's value types and events).
//
// A reader restores into an object built from the same configuration, so fixed capacities must
// match; a mismatch, a short input or an out-of-range value fails the reader (sticky), and the
// caller discards the half-restored object.

namespace jarvis::core {

class StateWriter {
public:
  static constexpr bool kReading = false;

  explicit StateWriter(std::vector<std::byte>& out) noexcept : out_{&out} {}

  void u8(std::uint8_t v) { out_->push_back(static_cast<std::byte>(v)); }
  void u16(std::uint16_t v) { put(v, 2); }
  void u32(std::uint32_t v) { put(v, 4); }
  void u64(std::uint64_t v) { put(v, 8); }
  void raw(std::span<const std::byte> bytes) {
    out_->insert(out_->end(), bytes.begin(), bytes.end());
  }

  void fail(Status status) noexcept {
    if (ok(status_)) {
      status_ = status;
    }
  }
  [[nodiscard]] Status status() const noexcept { return status_; }
  [[nodiscard]] bool ok_state() const noexcept { return ok(status_); }
  [[nodiscard]] std::size_t size() const noexcept { return out_->size(); }

  template <typename... T> void operator()(T&... values);

private:
  void put(std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      out_->push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
    }
  }

  std::vector<std::byte>* out_;
  Status status_ = Status::Ok;
};

class StateReader {
public:
  static constexpr bool kReading = true;

  explicit StateReader(std::span<const std::byte> in) noexcept : in_{in} {}

  std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(get(1)); }
  std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(get(2)); }
  std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(get(4)); }
  std::uint64_t u64() noexcept { return get(8); }
  std::span<const std::byte> raw(std::size_t n) noexcept {
    if (!available(n)) {
      return {};
    }
    const std::span<const std::byte> out = in_.subspan(pos_, n);
    pos_ += n;
    return out;
  }
  // What is left, for decoders that report how much they consumed (skip()).
  [[nodiscard]] std::span<const std::byte> rest() const noexcept { return in_.subspan(pos_); }
  void skip(std::size_t n) noexcept { static_cast<void>(raw(n)); }

  void fail(Status status) noexcept {
    if (ok(status_)) {
      status_ = status;
    }
  }
  void check(Status status) noexcept {
    if (!ok(status)) {
      fail(status);
    }
  }
  [[nodiscard]] Status status() const noexcept { return status_; }
  [[nodiscard]] bool ok_state() const noexcept { return ok(status_); }
  [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

  template <typename... T> void operator()(T&... values);

private:
  bool available(std::size_t n) noexcept {
    if (!ok(status_) || n > in_.size() - pos_) {
      fail(Status::Truncated);
      return false;
    }
    return true;
  }
  std::uint64_t get(std::size_t n) noexcept {
    if (!available(n)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
      v |= static_cast<std::uint64_t>(in_[pos_ + i]) << (8U * i);
    }
    pos_ += n;
    return v;
  }

  std::span<const std::byte> in_;
  std::size_t pos_ = 0;
  Status status_ = Status::Ok;
};

template <typename A>
concept StateArchive = std::is_same_v<A, StateWriter> || std::is_same_v<A, StateReader>;

// Container lengths are u32; nothing in the kernel holds more.
inline constexpr std::uint32_t kMaxStateElements = 1U << 28U;

template <StateArchive Ar, typename T> void io(Ar& ar, T& value);

namespace state_detail {

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};

// Integers of `bytes` bytes, as unsigned.
template <typename T> void integer(StateWriter& w, const T& v) {
  using U = std::make_unsigned_t<T>;
  const auto u = static_cast<U>(v);
  if constexpr (sizeof(T) == 1) {
    w.u8(static_cast<std::uint8_t>(u));
  } else if constexpr (sizeof(T) == 2) {
    w.u16(static_cast<std::uint16_t>(u));
  } else if constexpr (sizeof(T) == 4) {
    w.u32(static_cast<std::uint32_t>(u));
  } else {
    static_assert(sizeof(T) == 8);
    w.u64(static_cast<std::uint64_t>(u));
  }
}
template <typename T> void integer(StateReader& r, T& v) {
  using U = std::make_unsigned_t<T>;
  U u = 0;
  if constexpr (sizeof(T) == 1) {
    u = static_cast<U>(r.u8());
  } else if constexpr (sizeof(T) == 2) {
    u = static_cast<U>(r.u16());
  } else if constexpr (sizeof(T) == 4) {
    u = static_cast<U>(r.u32());
  } else {
    static_assert(sizeof(T) == 8);
    u = static_cast<U>(r.u64());
  }
  v = static_cast<T>(u);
}

inline void wide(StateWriter& w, u128 v) {
  w.u64(static_cast<std::uint64_t>(v));
  w.u64(static_cast<std::uint64_t>(v >> 64U));
}
inline void wide(StateReader& r, u128& v) {
  const std::uint64_t lo = r.u64();
  const std::uint64_t hi = r.u64();
  v = (static_cast<u128>(hi) << 64U) | lo;
}

} // namespace state_detail

// A length written before the elements; the reader checks it against `capacity`.
inline void state_length(StateWriter& w, std::size_t n) { w.u32(static_cast<std::uint32_t>(n)); }
[[nodiscard]] inline std::size_t state_length(StateReader& r, std::size_t capacity) {
  const std::uint32_t n = r.u32();
  if (n > capacity || n > kMaxStateElements) {
    r.fail(Status::CapacityExceeded);
    return 0;
  }
  return n;
}

// A vector of u64 that is mostly zeros (the levels of an order book's window), as runs: the
// capacity and length, then (zeros u32, values u32, the values) until the length is covered.
// Reading restores the same length and capacity check as FixedVector::state.
template <typename V> void state_sparse(StateWriter& w, V& v) {
  const std::size_t n = v.size();
  w.u32(static_cast<std::uint32_t>(v.capacity()));
  w.u32(static_cast<std::uint32_t>(n));
  std::size_t i = 0;
  while (i < n) {
    std::size_t zeros = 0;
    while (i + zeros < n && v[i + zeros] == 0) {
      ++zeros;
    }
    std::size_t values = 0;
    while (i + zeros + values < n && v[i + zeros + values] != 0) {
      ++values;
    }
    w.u32(static_cast<std::uint32_t>(zeros));
    w.u32(static_cast<std::uint32_t>(values));
    for (std::size_t k = 0; k < values; ++k) {
      w.u64(v[i + zeros + k]);
    }
    i += zeros + values;
  }
}
template <typename V> void state_sparse(StateReader& r, V& v) {
  const std::uint32_t capacity = r.u32();
  const std::uint32_t n = r.u32();
  if (!r.ok_state() || capacity != v.capacity() || n != v.size()) {
    r.fail(Status::CapacityExceeded);
    return;
  }
  std::size_t i = 0;
  while (i < n && r.ok_state()) {
    const std::uint32_t zeros = r.u32();
    const std::uint32_t values = r.u32();
    if (static_cast<std::size_t>(zeros) + values > n - i || zeros + values == 0) {
      r.fail(Status::InvalidArgument);
      return;
    }
    for (std::uint32_t k = 0; k < zeros; ++k) {
      v[i++] = 0;
    }
    for (std::uint32_t k = 0; k < values; ++k) {
      v[i++] = r.u64();
    }
  }
}

// ---- core and standard types ----------------------------------------------------------------

template <StateArchive Ar> void state_io(Ar& ar, UnixNanos& v) {
  std::uint64_t ns = v.value();
  io(ar, ns);
  if constexpr (Ar::kReading) {
    v = UnixNanos{ns};
  }
}
template <StateArchive Ar> void state_io(Ar& ar, DurationNanos& v) {
  std::uint64_t ns = v.value();
  io(ar, ns);
  if constexpr (Ar::kReading) {
    v = DurationNanos{ns};
  }
}
template <StateArchive Ar> void state_io(Ar& ar, EventKey& k) { ar(k.ts, k.source_id, k.seq); }

template <std::size_t N> void state_io(StateWriter& w, FixedString<N>& s) {
  const std::string_view text = s.view();
  w.u8(static_cast<std::uint8_t>(text.size()));
  w.raw(std::as_bytes(std::span<const char>{text.data(), text.size()}));
}
template <std::size_t N> void state_io(StateReader& r, FixedString<N>& s) {
  const std::size_t n = r.u8();
  const std::span<const std::byte> bytes = r.raw(n);
  if (!r.ok_state()) {
    return;
  }
  const std::string_view text{reinterpret_cast<const char*>(bytes.data()), n}; // NOLINT
  r.check(FixedString<N>::from(text, s));
}

template <StateArchive Ar, typename T> void state_io(Ar& ar, std::optional<T>& v) {
  bool has = v.has_value();
  io(ar, has);
  if constexpr (Ar::kReading) {
    if (!has) {
      v.reset();
      return;
    }
    if (!v.has_value()) {
      v.emplace();
    }
  }
  if (has) {
    io(ar, *v);
  }
}

template <StateArchive Ar, typename T, std::size_t N> void state_io(Ar& ar, std::array<T, N>& a) {
  for (T& item : a) {
    io(ar, item);
  }
}

template <StateArchive Ar, typename A, typename B> void state_io(Ar& ar, std::pair<A, B>& p) {
  ar(p.first, p.second);
}

namespace state_detail {
template <std::size_t I, typename... T> void emplace_index(std::variant<T...>& v, std::size_t i) {
  if constexpr (I < sizeof...(T)) {
    if (i == I) {
      v.template emplace<I>();
    } else {
      emplace_index<I + 1>(v, i);
    }
  }
}
} // namespace state_detail

template <StateArchive Ar, typename... T> void state_io(Ar& ar, std::variant<T...>& v) {
  std::uint8_t index = static_cast<std::uint8_t>(v.index());
  io(ar, index);
  if constexpr (Ar::kReading) {
    if (index >= sizeof...(T)) {
      ar.fail(Status::InvalidArgument);
      return;
    }
    if (v.index() != index) {
      state_detail::emplace_index<0>(v, index);
    }
  }
  std::visit([&ar](auto& alternative) { io(ar, alternative); }, v);
}

// std::monostate: nothing.
template <StateArchive Ar> void state_io(Ar& /*ar*/, std::monostate& /*v*/) {}

// ---- dispatch -------------------------------------------------------------------------------

template <StateArchive Ar, typename T> void io(Ar& ar, T& value) {
  if constexpr (requires { value.state(ar); }) {
    value.state(ar);
  } else if constexpr (std::is_same_v<T, bool>) {
    if constexpr (Ar::kReading) {
      const std::uint8_t b = ar.u8();
      if (b > 1) {
        ar.fail(Status::InvalidArgument);
      }
      value = b == 1;
    } else {
      ar.u8(value ? 1U : 0U);
    }
  } else if constexpr (std::is_enum_v<T>) {
    auto u = static_cast<std::underlying_type_t<T>>(value);
    state_detail::integer(ar, u);
    if constexpr (Ar::kReading) {
      value = static_cast<T>(u);
    }
  } else if constexpr (std::is_same_v<T, u128>) {
    state_detail::wide(ar, value);
  } else if constexpr (std::is_same_v<T, i128>) {
    auto u = static_cast<u128>(value);
    state_detail::wide(ar, u);
    if constexpr (Ar::kReading) {
      value = static_cast<i128>(u);
    }
  } else if constexpr (std::is_integral_v<T>) {
    state_detail::integer(ar, value);
  } else {
    state_io(ar, value);
  }
}

template <typename... T> void StateWriter::operator()(T&... values) { (io(*this, values), ...); }
template <typename... T> void StateReader::operator()(T&... values) { (io(*this, values), ...); }

// Encodes `value` (not modified) into `out`.
template <typename T> [[nodiscard]] Status save_state(const T& value, std::vector<std::byte>& out) {
  StateWriter w{out};
  io(w, const_cast<T&>(value)); // NOLINT(cppcoreguidelines-pro-type-const-cast): read only
  return w.status();
}

// Restores `value` from all of `in`; bytes left over are an error.
template <typename T> [[nodiscard]] Status load_state(T& value, std::span<const std::byte> in) {
  StateReader r{in};
  io(r, value);
  if (r.ok_state() && r.remaining() != 0) {
    return Status::InvalidArgument;
  }
  return r.status();
}

} // namespace jarvis::core
