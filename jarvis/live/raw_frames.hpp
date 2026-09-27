#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"

// The raw frame file (docs/architecture.md section 13.4): every WebSocket message exactly as it
// arrived, with its arrival time and connection, plus connection open and close marks. It is
// what `jarvis redecode` rebuilds the decoded log from, the fuzz corpus source, and the capture
// format for codec fixtures. Little-endian throughout:
//
//   header  "JVRAWFR1" (8 bytes), u32 version = 1, u32 reserved = 0
//   record  u64 recv_ns, u32 conn_id, u8 kind, u8 opcode, u16 reserved, u32 length, bytes
//
// kind: Open (bytes = the URL), Message (opcode = the WebSocket opcode), Close (bytes = the
// reason). recv_ns is UTC nanoseconds taken from a monotonic clock anchored once at start.

namespace jarvis::live {

enum class RawKind : std::uint8_t { Open = 0, Message = 1, Close = 2 };

struct RawFrame {
  std::uint64_t recv_ns = 0;
  std::uint32_t conn_id = 0;
  RawKind kind = RawKind::Message;
  std::uint8_t opcode = 0;
  std::span<const std::byte> bytes;

  [[nodiscard]] std::string_view text() const noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; // NOLINT
  }
};

inline constexpr std::size_t kRawHeaderSize = 16;
inline constexpr std::size_t kRawRecordHeaderSize = 24;
inline constexpr std::uint32_t kRawMaxLength = 64U << 20U;

class RawFrameWriter {
public:
  RawFrameWriter() = default;
  ~RawFrameWriter();
  RawFrameWriter(const RawFrameWriter&) = delete;
  RawFrameWriter& operator=(const RawFrameWriter&) = delete;

  [[nodiscard]] core::Status open(const std::string& path, std::string& error);
  [[nodiscard]] core::Status write(const RawFrame& frame);
  [[nodiscard]] core::Status close();
  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
  std::FILE* file_ = nullptr;
  std::uint64_t records_ = 0;
  std::uint64_t bytes_ = 0;
};

// Reads a whole file into memory and iterates its records; frames point into the reader.
class RawFrameReader {
public:
  [[nodiscard]] core::Status open(const std::string& path, std::string& error);
  // Ok with the next frame; EndOfStream after the last; Truncated or ParseError on a damaged
  // file (the offset says where).
  [[nodiscard]] core::Status next(RawFrame& out);
  [[nodiscard]] std::size_t offset() const noexcept { return at_; }

private:
  std::vector<std::byte> data_;
  std::size_t at_ = 0;
};

// UTC nanoseconds from a monotonic clock: the wall clock is read once, at construction, so
// arrival times never go backwards when the wall clock is adjusted.
class ArrivalClock {
public:
  ArrivalClock();
  // A steady-clock reading (network::steady_ns) in UTC nanoseconds.
  [[nodiscard]] std::uint64_t utc_of(std::int64_t steady_ns) const noexcept;
  [[nodiscard]] std::uint64_t now() const noexcept;

private:
  std::int64_t offset_ns_ = 0;
};

} // namespace jarvis::live
