#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jarvis::core {

// SHA-256 (FIPS 180-4). Used for the configuration hash in the event log header and for the
// summary line of fingerprints; determinism is decided by byte comparison, not by the digest.
class Sha256 {
public:
  using Digest = std::array<std::uint8_t, 32>;

  constexpr Sha256() noexcept = default;

  constexpr void update(std::span<const std::byte> data) noexcept {
    for (const std::byte b : data) {
      block_[block_size_++] = static_cast<std::uint8_t>(b);
      if (block_size_ == 64) {
        compress();
        block_size_ = 0;
      }
    }
    bit_length_ += static_cast<std::uint64_t>(data.size()) * 8U;
  }

  [[nodiscard]] constexpr Digest finish() noexcept {
    const std::uint64_t length = bit_length_;
    block_[block_size_++] = 0x80U;
    if (block_size_ > 56) {
      while (block_size_ < 64) {
        block_[block_size_++] = 0;
      }
      compress();
      block_size_ = 0;
    }
    while (block_size_ < 56) {
      block_[block_size_++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
      block_[block_size_++] = static_cast<std::uint8_t>(length >> (static_cast<unsigned>(i) * 8U));
    }
    compress();
    Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24U);
      digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16U);
      digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8U);
      digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
  }

private:
  static constexpr std::array<std::uint32_t, 64> kRound = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};

  [[nodiscard]] static constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32U - n));
  }

  constexpr void compress() noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block_[i * 4]) << 24U) |
             (static_cast<std::uint32_t>(block_[i * 4 + 1]) << 16U) |
             (static_cast<std::uint32_t>(block_[i * 4 + 2]) << 8U) |
             static_cast<std::uint32_t>(block_[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<std::uint32_t, 8> v = state_;
    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint32_t t1 = v[7] + s1 + ch + kRound[i] + w[i];
      const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const std::uint32_t t2 = s0 + maj;
      v[7] = v[6];
      v[6] = v[5];
      v[5] = v[4];
      v[4] = v[3] + t1;
      v[3] = v[2];
      v[2] = v[1];
      v[1] = v[0];
      v[0] = t1 + t2;
    }
    for (std::size_t i = 0; i < 8; ++i) {
      state_[i] += v[i];
    }
  }

  std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t bit_length_ = 0;
};

} // namespace jarvis::core
