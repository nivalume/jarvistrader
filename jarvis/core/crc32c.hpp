#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

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

namespace detail {

[[nodiscard]] constexpr std::uint32_t crc32c_table(std::uint32_t state,
                                                   std::span<const std::byte> data) noexcept {
  for (const std::byte b : data) {
    state = kCrc32cTable[(state ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (state >> 8U);
  }
  return state;
}

// Hardware CRC-32C: the SSE4.2 crc32 instruction on x86-64, the CRC extension on ARMv8. Same
// polynomial and bit order as the table, so results are identical; only speed differs.
#if defined(__x86_64__)
inline constexpr bool kHasCrc32cInstruction = true;

[[nodiscard]] inline bool crc32c_instruction_available() noexcept {
  // GCC returns int and Clang returns bool, so the cast is redundant only under Clang.
  // NOLINTNEXTLINE(readability-redundant-casting)
  static const bool available = static_cast<bool>(__builtin_cpu_supports("sse4.2"));
  return available;
}

[[gnu::target("sse4.2")]] [[nodiscard]] inline std::uint32_t
crc32c_instruction(std::uint32_t state, std::span<const std::byte> data) noexcept {
  const std::byte* p = data.data();
  std::size_t n = data.size();
  std::uint64_t wide = state;
  for (; n >= 8; n -= 8, p += 8) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::uint64_t word = 0;
    __builtin_memcpy(&word, p, 8);
    wide = __builtin_ia32_crc32di(wide, word);
  }
  auto narrow = static_cast<std::uint32_t>(wide);
  for (; n > 0; --n, ++p) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    narrow = __builtin_ia32_crc32qi(narrow, static_cast<unsigned char>(*p));
  }
  return narrow;
}
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
inline constexpr bool kHasCrc32cInstruction = true;

[[nodiscard]] inline bool crc32c_instruction_available() noexcept { return true; }

[[nodiscard]] inline std::uint32_t crc32c_instruction(std::uint32_t state,
                                                      std::span<const std::byte> data) noexcept {
  const std::byte* p = data.data();
  std::size_t n = data.size();
  for (; n >= 8; n -= 8, p += 8) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::uint64_t word = 0;
    __builtin_memcpy(&word, p, 8);
    state = __builtin_arm_crc32cd(state, word);
  }
  for (; n > 0; --n, ++p) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    state = __builtin_arm_crc32cb(state, static_cast<unsigned char>(*p));
  }
  return state;
}
#else
inline constexpr bool kHasCrc32cInstruction = false;

[[nodiscard]] inline bool crc32c_instruction_available() noexcept { return false; }

[[nodiscard]] inline std::uint32_t crc32c_instruction(std::uint32_t state,
                                                      std::span<const std::byte> data) noexcept {
  return crc32c_table(state, data);
}
#endif

} // namespace detail

[[nodiscard]] constexpr std::uint32_t crc32c_extend(std::uint32_t state,
                                                    std::span<const std::byte> data) noexcept {
  if (!std::is_constant_evaluated() && detail::crc32c_instruction_available()) {
    return detail::crc32c_instruction(state, data);
  }
  return detail::crc32c_table(state, data);
}

[[nodiscard]] constexpr std::uint32_t crc32c_finish(std::uint32_t state) noexcept {
  return state ^ 0xFFFFFFFFU;
}

[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_finish(crc32c_extend(crc32c_init(), data));
}

} // namespace jarvis::core
