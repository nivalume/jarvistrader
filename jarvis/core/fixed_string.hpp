#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "jarvis/core/status.hpp"

namespace jarvis::core {

// Inline string with a fixed capacity. Trivially copyable, so it can live in events, arenas and
// the event log without heap allocation. Comparison is by content.
template <std::size_t Capacity> class FixedString {
  static_assert(Capacity > 0 && Capacity <= 255, "FixedString capacity must fit in one byte");

public:
  constexpr FixedString() noexcept = default;

  [[nodiscard]] static constexpr Status from(std::string_view text, FixedString& out) noexcept {
    if (text.size() > Capacity) {
      return Status::OutOfRange;
    }
    FixedString value;
    for (std::size_t i = 0; i < text.size(); ++i) {
      value.data_[i] = text[i];
    }
    value.size_ = static_cast<std::uint8_t>(text.size());
    out = value;
    return Status::Ok;
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return std::string_view{data_.data(), size_};
  }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  friend constexpr bool operator==(const FixedString& a, const FixedString& b) noexcept {
    return a.view() == b.view();
  }
  friend constexpr std::strong_ordering operator<=>(const FixedString& a,
                                                    const FixedString& b) noexcept {
    const int c = a.view().compare(b.view());
    if (c < 0) {
      return std::strong_ordering::less;
    }
    if (c > 0) {
      return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }

private:
  std::array<char, Capacity> data_{};
  std::uint8_t size_ = 0;
};

} // namespace jarvis::core
