#include "jarvis/node/event_log.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace jarvis::node {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

constexpr std::size_t kFlushThreshold = 1U << 20U;
constexpr std::size_t kMaxRecordBytes =
    wire::kRecordHeaderSize + wire::kMaxPayload + wire::kRecordTrailerSize;
constexpr std::size_t kMaxHeaderBytes = 1024;

Status write_all(int fd, std::span<const std::byte> bytes) {
  while (!bytes.empty()) {
    const ssize_t n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IoError;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(n));
  }
  return Status::Ok;
}

Status sync_file(int fd) {
#if defined(__APPLE__)
  return ::fsync(fd) == 0 ? Status::Ok : Status::IoError;
#else
  return ::fdatasync(fd) == 0 ? Status::Ok : Status::IoError;
#endif
}

// A new directory entry survives a crash once the directory itself is synced.
Status sync_directory(const std::string& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return Status::IoError;
  }
  const Status s = ::fsync(fd) == 0 ? Status::Ok : Status::IoError;
  ::close(fd);
  return s;
}

bool same_log(const wire::LogHeader& a, const wire::LogHeader& b) {
  return a.format_version == b.format_version && a.schema_version == b.schema_version &&
         a.config_hash == b.config_hash && a.seed == b.seed &&
         a.python_hash_seed == b.python_hash_seed && a.git_commit == b.git_commit;
}

} // namespace

std::string segment_name(std::uint32_t index) {
  std::array<char, 32> name{};
  const int n = std::snprintf(name.data(), name.size(), "events-%06u.jlog", index);
  return std::string{name.data(), static_cast<std::size_t>(n)};
}

EventLogWriter::EventLogWriter(EventLogWriter&& other) noexcept
    : fd_{std::exchange(other.fd_, -1)}, directory_{std::move(other.directory_)},
      header_{other.header_}, options_{other.options_}, segment_size_{other.segment_size_},
      records_{other.records_}, position_{other.position_}, buffer_{std::move(other.buffer_)},
      scratch_{std::move(other.scratch_)} {}

EventLogWriter& EventLogWriter::operator=(EventLogWriter&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    fd_ = std::exchange(other.fd_, -1);
    directory_ = std::move(other.directory_);
    header_ = other.header_;
    options_ = other.options_;
    segment_size_ = other.segment_size_;
    records_ = other.records_;
    position_ = other.position_;
    buffer_ = std::move(other.buffer_);
    scratch_ = std::move(other.scratch_);
  }
  return *this;
}

EventLogWriter::~EventLogWriter() { static_cast<void>(close()); }

Status EventLogWriter::open(const std::string& directory, const wire::LogHeader& header,
                            EventLogOptions options) {
  if (fd_ >= 0) {
    return Status::InvalidState;
  }
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return Status::IoError;
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (entry.path().extension() == ".jlog") {
      return Status::AlreadyExists;
    }
  }
  directory_ = directory;
  header_ = header;
  header_.segment_index = 0;
  options_ = options;
  records_ = 0;
  position_ = 0;
  buffer_.clear();
  buffer_.reserve(kFlushThreshold + kMaxRecordBytes);
  scratch_.assign(kMaxRecordBytes, std::byte{0});
  return open_segment();
}

Status EventLogWriter::open_segment() {
  const std::string path = directory_ + "/" + segment_name(header_.segment_index);
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    return errno == EEXIST ? Status::AlreadyExists : Status::IoError;
  }
  if (options_.durable) {
    const Status s = sync_directory(directory_);
    if (!core::ok(s)) {
      return s;
    }
  }
  std::array<std::byte, kMaxHeaderBytes> bytes{};
  std::size_t written = 0;
  const Status s = wire::encode_header(header_, bytes, written);
  if (!core::ok(s)) {
    return s;
  }
  segment_size_ = written;
  buffer_.insert(buffer_.end(), bytes.begin(),
                 bytes.begin() + static_cast<std::ptrdiff_t>(written));
  return Status::Ok;
}

Status EventLogWriter::append(const core::EventKey& key, const model::Event& event) {
  std::size_t written = 0;
  const Status s = wire::encode_record(key, event, scratch_, written);
  if (!core::ok(s)) {
    return s;
  }
  return append_record(std::span<const std::byte>{scratch_.data(), written});
}

Status EventLogWriter::append_output(const core::EventKey& key, const model::Output& output) {
  std::size_t written = 0;
  const Status s = wire::encode_output_record(key, output, scratch_, written);
  if (!core::ok(s)) {
    return s;
  }
  return append_record(std::span<const std::byte>{scratch_.data(), written});
}

Status EventLogWriter::append_record(std::span<const std::byte> record) {
  if (fd_ < 0) {
    return Status::InvalidState;
  }
  if (segment_size_ + record.size() > options_.segment_bytes && records_ > 0) {
    Status s = flush();
    if (core::ok(s) && options_.durable && !options_.sync_on_flush) {
      s = sync_fd();
    }
    if (!core::ok(s)) {
      return s;
    }
    ::close(fd_);
    fd_ = -1;
    ++header_.segment_index;
    s = open_segment();
    if (!core::ok(s)) {
      return s;
    }
  }
  buffer_.insert(buffer_.end(), record.begin(), record.end());
  segment_size_ += record.size();
  position_ += record.size();
  ++records_;
  return buffer_.size() >= kFlushThreshold ? flush() : Status::Ok;
}

Status EventLogWriter::flush() {
  if (fd_ < 0) {
    return Status::InvalidState;
  }
  const Status s = write_all(fd_, buffer_);
  if (!core::ok(s)) {
    return s;
  }
  buffer_.clear();
  return options_.sync_on_flush ? sync_fd() : Status::Ok;
}

Status EventLogWriter::sync_fd() const { return sync_file(fd_); }

Status EventLogWriter::sync() {
  Status s = flush();
  if (core::ok(s) && !options_.sync_on_flush) {
    s = sync_fd();
  }
  return s;
}

Status EventLogWriter::close() {
  if (fd_ < 0) {
    return Status::Ok;
  }
  Status s = flush();
  if (core::ok(s) && options_.durable && !options_.sync_on_flush) {
    s = sync_fd();
  }
  ::close(fd_);
  fd_ = -1;
  return s;
}

EventLogReader::EventLogReader() : scratch_{wire::kMaxPayload / 16} {}

Status EventLogReader::open(const std::string& directory, EventLogReadOptions options) {
  options_ = options;
  torn_bytes_ = 0;
  torn_segment_.clear();
  segments_.clear();
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (entry.path().extension() == ".jlog") {
      segments_.push_back(entry.path().string());
    }
  }
  if (ec) {
    return Status::IoError;
  }
  if (segments_.empty()) {
    return Status::NotFound;
  }
  std::sort(segments_.begin(), segments_.end());
  return load_segment(0);
}

Status EventLogReader::load_segment(std::size_t index) {
  std::ifstream file(segments_[index], std::ios::binary | std::ios::ate);
  if (!file) {
    return Status::IoError;
  }
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return Status::IoError;
  }
  data_.resize(static_cast<std::size_t>(size));
  file.seekg(0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): std::byte storage read as chars
  if (!file.read(reinterpret_cast<char*>(data_.data()), size)) {
    return Status::IoError;
  }
  wire::LogHeader header;
  std::size_t consumed = 0;
  const Status s = wire::decode_header(data_, header, consumed);
  if (!core::ok(s)) {
    segment_ = index;
    // A crash right after the segment was created: nothing in it was ever a record.
    if (index > 0 && options_.tolerate_torn_tail && last_segment()) {
      return torn(0);
    }
    return s;
  }
  if (index == 0) {
    header_ = header;
  } else if (!same_log(header, header_) || header.segment_index != index) {
    return Status::InvalidArgument;
  }
  segment_ = index;
  pos_ = consumed;
  return Status::Ok;
}

Status EventLogReader::next(wire::RecordView& out) {
  while (pos_ >= data_.size()) {
    if (segment_ + 1 >= segments_.size()) {
      return Status::EndOfStream;
    }
    const Status s = load_segment(segment_ + 1);
    if (!core::ok(s)) {
      return s;
    }
  }
  const Status s = wire::decode_record(std::span<const std::byte>{data_}.subspan(pos_), out);
  if (!core::ok(s)) {
    if (options_.tolerate_torn_tail && last_segment()) {
      static_cast<void>(torn(pos_));
      return Status::EndOfStream;
    }
    return s;
  }
  pos_ += out.bytes.size();
  return Status::Ok;
}

Status EventLogReader::torn(std::size_t from) {
  torn_bytes_ = data_.size() - from;
  torn_segment_ = segments_[segment_];
  pos_ = data_.size();
  return Status::Ok;
}

Status EventLogReader::decode(const wire::RecordView& record, model::Event& out) {
  return wire::decode_event(record, scratch_, out);
}

Status EventLogReader::decode_output(const wire::RecordView& record, model::Output& out) {
  return wire::decode_output(record, out);
}

} // namespace jarvis::node
