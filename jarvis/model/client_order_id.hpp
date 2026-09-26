#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/identifiers.hpp"

namespace jarvis::model {

// jarvis ClientOrderId format (docs/architecture.md section 8.4):
//
//   {node_tag}-{epoch:6 base32}-{seq:8 base32}     e.g. "mm01-0001Q2-00000G4K"
//
// No wall-clock time, so ids are identical on replay. `epoch` increments every time a node
// starts (a persisted counter) and `seq` increments per order within the epoch, so an id is never
// reused, and an order left behind by an earlier epoch of the same node is recognizable during
// reconciliation. Uses Crockford base32 and fits Binance's ^[\.A-Z\:/a-z0-9_-]{1,36}$.

inline constexpr std::string_view kCrockfordBase32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
inline constexpr std::size_t kEpochDigits = 6;
inline constexpr std::size_t kSeqDigits = 8;
inline constexpr std::size_t kMaxNodeTag = 8;
inline constexpr std::uint64_t kMaxEpoch = (1ULL << (5U * kEpochDigits)) - 1;
inline constexpr std::uint64_t kMaxSeq = (1ULL << (5U * kSeqDigits)) - 1;

namespace detail {

[[nodiscard]] constexpr bool valid_node_tag(std::string_view tag) noexcept {
  if (tag.empty() || tag.size() > kMaxNodeTag) {
    return false;
  }
  return std::ranges::all_of(tag, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  });
}

[[nodiscard]] constexpr bool decode_base32(std::string_view text, std::uint64_t& out) noexcept {
  std::uint64_t value = 0;
  for (const char c : text) {
    const std::size_t digit = kCrockfordBase32.find(c);
    if (digit == std::string_view::npos) {
      return false;
    }
    value = value * 32U + digit;
  }
  out = value;
  return true;
}

} // namespace detail

struct DecodedClientOrderId {
  core::FixedString<kMaxNodeTag> node_tag;
  std::uint64_t epoch = 0;
  std::uint64_t seq = 0;
};

class ClientOrderIdGenerator {
public:
  // `node_tag`: 1-8 ASCII letters or digits. `epoch`: this node run's persisted counter.
  [[nodiscard]] static constexpr core::Status create(std::string_view node_tag, std::uint64_t epoch,
                                                     ClientOrderIdGenerator& out) noexcept {
    if (!detail::valid_node_tag(node_tag) || epoch > kMaxEpoch) {
      return core::Status::InvalidArgument;
    }
    ClientOrderIdGenerator g;
    static_cast<void>(core::FixedString<kMaxNodeTag>::from(node_tag, g.tag_));
    g.epoch_ = epoch;
    out = g;
    return core::Status::Ok;
  }

  [[nodiscard]] constexpr core::Status next(ClientOrderId& out) noexcept {
    if (seq_ >= kMaxSeq) {
      return core::Status::CapacityExceeded;
    }
    ++seq_;
    return format(tag_.view(), epoch_, seq_, out);
  }

  [[nodiscard]] constexpr std::uint64_t last_seq() const noexcept { return seq_; }

  [[nodiscard]] static constexpr core::Status format(std::string_view node_tag, std::uint64_t epoch,
                                                     std::uint64_t seq, ClientOrderId& out) noexcept {
    if (!detail::valid_node_tag(node_tag) || epoch > kMaxEpoch || seq > kMaxSeq) {
      return core::Status::InvalidArgument;
    }
    std::array<char, kMaxNodeTag + 2 + kEpochDigits + kSeqDigits> buffer{};
    std::size_t pos = 0;
    for (const char c : node_tag) {
      buffer[pos++] = c;
    }
    buffer[pos++] = '-';
    for (std::size_t i = kEpochDigits; i > 0; --i) {
      buffer[pos + i - 1] = kCrockfordBase32[epoch % 32U];
      epoch /= 32U;
    }
    pos += kEpochDigits;
    buffer[pos++] = '-';
    for (std::size_t i = kSeqDigits; i > 0; --i) {
      buffer[pos + i - 1] = kCrockfordBase32[seq % 32U];
      seq /= 32U;
    }
    pos += kSeqDigits;
    return ClientOrderId::from(std::string_view{buffer.data(), pos}, out);
  }

  // NotFound if `id` is not in the jarvis format (an external or foreign order).
  [[nodiscard]] static constexpr core::Status decode(const ClientOrderId& id,
                                                     DecodedClientOrderId& out) noexcept {
    const std::string_view text = id.view();
    const std::size_t second = text.rfind('-');
    if (second == std::string_view::npos || second == 0) {
      return core::Status::NotFound;
    }
    const std::size_t first = text.rfind('-', second - 1);
    if (first == std::string_view::npos || second - first - 1 != kEpochDigits ||
        text.size() - second - 1 != kSeqDigits) {
      return core::Status::NotFound;
    }
    DecodedClientOrderId decoded;
    const std::string_view tag = text.substr(0, first);
    if (!detail::valid_node_tag(tag) ||
        !detail::decode_base32(text.substr(first + 1, kEpochDigits), decoded.epoch) ||
        !detail::decode_base32(text.substr(second + 1), decoded.seq)) {
      return core::Status::NotFound;
    }
    static_cast<void>(core::FixedString<kMaxNodeTag>::from(tag, decoded.node_tag));
    out = decoded;
    return core::Status::Ok;
  }

private:
  core::FixedString<kMaxNodeTag> tag_;
  std::uint64_t epoch_ = 0;
  std::uint64_t seq_ = 0;
};

} // namespace jarvis::model
