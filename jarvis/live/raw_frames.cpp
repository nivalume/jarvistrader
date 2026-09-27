#include "jarvis/live/raw_frames.hpp"

#include <array>
#include <chrono>
#include <cstring>

namespace jarvis::live {

namespace {

constexpr std::array<char, 8> kMagic{'J', 'V', 'R', 'A', 'W', 'F', 'R', '1'};
constexpr std::uint32_t kVersion = 1;

template <typename T> void put(std::byte* out, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8U * i)) & 0xFFU);
  }
}

template <typename T> T get(const std::byte* in) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    v |= std::to_integer<std::uint64_t>(in[i]) << (8U * i);
  }
  return static_cast<T>(v);
}

std::int64_t steady_now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

RawFrameWriter::~RawFrameWriter() { (void)close(); }

core::Status RawFrameWriter::open(const std::string& path, std::string& error) {
  if (file_ != nullptr) {
    error = "already open";
    return core::Status::InvalidState;
  }
  file_ = std::fopen(path.c_str(), "wb"); // NOLINT(cppcoreguidelines-owning-memory)
  if (file_ == nullptr) {
    error = "cannot create " + path + ": " + std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
    return core::Status::IoError;
  }
  std::array<std::byte, kRawHeaderSize> header{};
  std::memcpy(header.data(), kMagic.data(), kMagic.size());
  put<std::uint32_t>(header.data() + 8, kVersion);
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    error = "cannot write " + path;
    return core::Status::IoError;
  }
  records_ = 0;
  bytes_ = header.size();
  return core::Status::Ok;
}

core::Status RawFrameWriter::write(const RawFrame& frame) {
  if (file_ == nullptr) {
    return core::Status::InvalidState;
  }
  if (frame.bytes.size() > kRawMaxLength) {
    return core::Status::CapacityExceeded;
  }
  std::array<std::byte, kRawRecordHeaderSize> h{};
  put<std::uint64_t>(h.data(), frame.recv_ns);
  put<std::uint32_t>(h.data() + 8, frame.conn_id);
  h[12] = static_cast<std::byte>(frame.kind);
  h[13] = static_cast<std::byte>(frame.opcode);
  put<std::uint32_t>(h.data() + 16, static_cast<std::uint32_t>(frame.bytes.size()));
  if (std::fwrite(h.data(), 1, h.size(), file_) != h.size() ||
      std::fwrite(frame.bytes.data(), 1, frame.bytes.size(), file_) != frame.bytes.size()) {
    return core::Status::IoError;
  }
  ++records_;
  bytes_ += h.size() + frame.bytes.size();
  return core::Status::Ok;
}

core::Status RawFrameWriter::close() {
  if (file_ == nullptr) {
    return core::Status::Ok;
  }
  const bool ok = std::fclose(file_) == 0; // NOLINT(cppcoreguidelines-owning-memory)
  file_ = nullptr;
  return ok ? core::Status::Ok : core::Status::IoError;
}

core::Status RawFrameReader::open(const std::string& path, std::string& error) {
  std::FILE* f = std::fopen(path.c_str(), "rb"); // NOLINT(cppcoreguidelines-owning-memory)
  if (f == nullptr) {
    error = "cannot open " + path + ": " + std::strerror(errno); // NOLINT(concurrency-mt-unsafe)
    return core::Status::NotFound;
  }
  data_.clear();
  std::array<std::byte, 1U << 16U> chunk{};
  for (;;) {
    const std::size_t n = std::fread(chunk.data(), 1, chunk.size(), f);
    data_.insert(data_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
    if (n < chunk.size()) {
      break;
    }
  }
  const bool read_error = std::ferror(f) != 0;
  std::fclose(f); // NOLINT(cppcoreguidelines-owning-memory)
  if (read_error) {
    error = "cannot read " + path;
    return core::Status::IoError;
  }
  if (data_.size() < kRawHeaderSize ||
      std::memcmp(data_.data(), kMagic.data(), kMagic.size()) != 0) {
    error = path + " is not a raw frame file";
    return core::Status::ParseError;
  }
  if (get<std::uint32_t>(data_.data() + 8) != kVersion) {
    error = path + ": unsupported raw frame file version";
    return core::Status::UnsupportedMessage;
  }
  at_ = kRawHeaderSize;
  return core::Status::Ok;
}

core::Status RawFrameReader::next(RawFrame& out) {
  if (at_ == data_.size()) {
    return core::Status::EndOfStream;
  }
  if (data_.size() - at_ < kRawRecordHeaderSize) {
    return core::Status::Truncated;
  }
  const std::byte* h = data_.data() + at_;
  const auto kind = std::to_integer<std::uint8_t>(h[12]);
  const auto length = get<std::uint32_t>(h + 16);
  if (kind > static_cast<std::uint8_t>(RawKind::Close) || length > kRawMaxLength) {
    return core::Status::ParseError;
  }
  if (data_.size() - at_ - kRawRecordHeaderSize < length) {
    return core::Status::Truncated;
  }
  out.recv_ns = get<std::uint64_t>(h);
  out.conn_id = get<std::uint32_t>(h + 8);
  out.kind = static_cast<RawKind>(kind);
  out.opcode = std::to_integer<std::uint8_t>(h[13]);
  out.bytes = std::span<const std::byte>{h + kRawRecordHeaderSize, length};
  at_ += kRawRecordHeaderSize + length;
  return core::Status::Ok;
}

ArrivalClock::ArrivalClock() {
  const std::int64_t wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  offset_ns_ = wall - steady_now();
}

std::uint64_t ArrivalClock::utc_of(std::int64_t steady_ns) const noexcept {
  return static_cast<std::uint64_t>(steady_ns + offset_ns_);
}

std::uint64_t ArrivalClock::now() const noexcept { return utc_of(steady_now()); }

} // namespace jarvis::live
