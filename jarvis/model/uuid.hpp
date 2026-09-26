#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "jarvis/core/rng.hpp"
#include "jarvis/core/status.hpp"

namespace jarvis::model {

// RFC 4122 version 4 UUID (nautilus `UUID4`, used for event ids). The kernel derives event ids
// from the counter-based generator instead of an entropy source, so they replay identically.
class Uuid4 {
public:
  using Bytes = std::array<std::uint8_t, 16>;
  using Text = std::array<char, 36>;

  constexpr Uuid4() noexcept = default;

  [[nodiscard]] static constexpr Uuid4 derive(const core::CounterRng& rng, std::uint64_t identity,
                                              std::uint32_t hop) noexcept {
    Uuid4 id;
    const std::uint64_t hi = rng.draw(identity, hop, 0);
    const std::uint64_t lo = rng.draw(identity, hop, 1);
    for (std::size_t i = 0; i < 8; ++i) {
      id.bytes_[i] = static_cast<std::uint8_t>(hi >> (56U - 8U * i));
      id.bytes_[8 + i] = static_cast<std::uint8_t>(lo >> (56U - 8U * i));
    }
    id.bytes_[6] = static_cast<std::uint8_t>((id.bytes_[6] & 0x0FU) | 0x40U); // version 4
    id.bytes_[8] = static_cast<std::uint8_t>((id.bytes_[8] & 0x3FU) | 0x80U); // RFC 4122 variant
    return id;
  }

  [[nodiscard]] static constexpr core::Status from_bytes(const Bytes& bytes, Uuid4& out) noexcept {
    if ((bytes[6] & 0xF0U) != 0x40U || (bytes[8] & 0xC0U) != 0x80U) {
      return core::Status::InvalidArgument;
    }
    out.bytes_ = bytes;
    return core::Status::Ok;
  }

  // 8-4-4-4-12 hexadecimal digits; must be version 4 with the RFC 4122 variant.
  [[nodiscard]] static constexpr core::Status parse(std::string_view text, Uuid4& out) noexcept {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
        text[23] != '-') {
      return core::Status::ParseError;
    }
    Bytes bytes{};
    std::size_t nibble = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        continue;
      }
      const char c = text[i];
      std::uint8_t v = 0;
      if (c >= '0' && c <= '9') {
        v = static_cast<std::uint8_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v = static_cast<std::uint8_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v = static_cast<std::uint8_t>(c - 'A' + 10);
      } else {
        return core::Status::ParseError;
      }
      bytes[nibble / 2] = static_cast<std::uint8_t>(bytes[nibble / 2] | (nibble % 2 == 0 ? v << 4U : v));
      ++nibble;
    }
    return from_bytes(bytes, out);
  }

  [[nodiscard]] constexpr Text text() const noexcept {
    constexpr std::string_view kDigits = "0123456789abcdef";
    Text out{};
    std::size_t pos = 0;
    for (std::size_t i = 0; i < 16; ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) {
        out[pos++] = '-';
      }
      out[pos++] = kDigits[bytes_[i] >> 4U];
      out[pos++] = kDigits[bytes_[i] & 0x0FU];
    }
    return out;
  }

  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept {
    return std::ranges::all_of(bytes_, [](std::uint8_t b) { return b == 0; });
  }

  friend constexpr bool operator==(const Uuid4&, const Uuid4&) noexcept = default;
  friend constexpr auto operator<=>(const Uuid4&, const Uuid4&) noexcept = default;

private:
  Bytes bytes_{};
};

} // namespace jarvis::model
