#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jarvis::core {

namespace detail {

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0x82F63B78U : crc >> 1U;
    }
    table[i] = crc;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

} // namespace detail

// CRC-32C (Castagnoli), as used for event log records. `crc32c_extend` continues a running
// checksum; start from crc32c_init() and finish with crc32c_finish().
[[nodiscard]] constexpr std::uint32_t crc32c_init() noexcept { return 0xFFFFFFFFU; }

[[nodiscard]] constexpr std::uint32_t crc32c_extend(std::uint32_t state,
                                                    std::span<const std::byte> data) noexcept {
  for (const std::byte b : data) {
    state = detail::kCrc32cTable[(state ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (state >> 8U);
  }
  return state;
}

[[nodiscard]] constexpr std::uint32_t crc32c_finish(std::uint32_t state) noexcept {
  return state ^ 0xFFFFFFFFU;
}

[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_finish(crc32c_extend(crc32c_init(), data));
}

} // namespace jarvis::core
