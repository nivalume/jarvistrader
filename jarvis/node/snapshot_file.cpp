#include "jarvis/node/snapshot_file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

#include "jarvis/core/crc32c.hpp"
#include "jarvis/node/build_info.hpp"

namespace jarvis::node {

namespace wire = jarvis::model::wire;
using core::Status;

namespace {

constexpr std::array<char, 8> kMagic = {'J', 'A', 'R', 'V', 'I', 'S', 'S', 'N'};
constexpr std::string_view kPrefix = "snapshot-";
constexpr std::string_view kSuffix = ".jsnap";
constexpr std::size_t kMaxHeaderBytes = 1024;

void put(std::vector<std::byte>& out, std::uint64_t v, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
  }
}

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

Status sync_directory(const std::string& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return Status::IoError;
  }
  const Status s = ::fsync(fd) == 0 ? Status::Ok : Status::IoError;
  ::close(fd);
  return s;
}

} // namespace

std::string snapshot_name(std::uint64_t seq) {
  std::array<char, 48> name{};
  const int n = std::snprintf(name.data(), name.size(), "snapshot-%020llu.jsnap",
                              static_cast<unsigned long long>(seq)); // NOLINT(google-runtime-int)
  return std::string{name.data(), static_cast<std::size_t>(n)};
}

Status encode_snapshot(const wire::LogHeader& header, const SnapshotInfo& info,
                       std::span<const std::byte> body, std::vector<std::byte>& out) {
  out.clear();
  out.reserve(body.size() + kMaxHeaderBytes);
  for (const char c : kMagic) {
    out.push_back(static_cast<std::byte>(c));
  }
  put(out, kSnapshotFormat, 2);
  std::array<std::byte, kMaxHeaderBytes> log_header{};
  std::size_t written = 0;
  wire::LogHeader h = header;
  h.segment_index = 0;
  const Status s = wire::encode_header(h, log_header, written);
  if (!core::ok(s)) {
    return s;
  }
  out.insert(out.end(), log_header.begin(),
             log_header.begin() + static_cast<std::ptrdiff_t>(written));
  put(out, info.seq, 8);
  put(out, info.ts, 8);
  put(out, info.complete ? 1U : 0U, 1);
  put(out, info.strategies, 2);
  put(out, body.size(), 8);
  out.insert(out.end(), body.begin(), body.end());
  put(out, core::crc32c(out), 4);
  return Status::Ok;
}

Status decode_snapshot(std::span<const std::byte> file, wire::LogHeader& header, SnapshotInfo& info,
                       std::span<const std::byte>& body) {
  if (file.size() < kMagic.size() + 2 + 4) {
    return Status::Truncated;
  }
  wire::Reader crc{file.subspan(file.size() - 4)};
  if (crc.u32() != core::crc32c(file.first(file.size() - 4))) {
    return Status::ChecksumMismatch;
  }
  const std::span<const std::byte> content = file.first(file.size() - 4);
  wire::Reader r{content};
  for (const char c : kMagic) {
    if (r.u8() != static_cast<std::uint8_t>(c)) {
      return Status::InvalidArgument;
    }
  }
  if (r.u16() != kSnapshotFormat) {
    return Status::UnsupportedMessage;
  }
  const std::size_t at = kMagic.size() + 2;
  std::size_t consumed = 0;
  const Status s = wire::decode_header(content.subspan(at), header, consumed);
  if (!core::ok(s)) {
    return s;
  }
  wire::Reader rest{content.subspan(at + consumed)};
  info.seq = rest.u64();
  info.ts = rest.u64();
  const std::uint8_t complete = rest.u8();
  info.complete = complete == 1;
  info.strategies = rest.u16();
  const std::uint64_t length = rest.u64();
  if (!rest.ok()) {
    return rest.status();
  }
  if (complete > 1 || length != rest.remaining()) {
    return Status::InvalidArgument;
  }
  body = rest.raw(static_cast<std::size_t>(length));
  return rest.status();
}

Status write_snapshot_file(const std::string& directory, std::uint64_t seq,
                           std::span<const std::byte> encoded, bool durable) {
  const std::string path = directory + "/" + snapshot_name(seq);
  const std::string temp = path + ".tmp";
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return Status::IoError;
  }
  Status s = write_all(fd, encoded);
  if (core::ok(s) && durable && ::fsync(fd) != 0) {
    s = Status::IoError;
  }
  ::close(fd);
  if (!core::ok(s)) {
    ::unlink(temp.c_str());
    return s;
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    return Status::IoError;
  }
  return durable ? sync_directory(directory) : Status::Ok;
}

Status read_file_bytes(const std::string& path, std::vector<std::byte>& out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return Status::NotFound;
  }
  const std::streamoff size = file.tellg();
  if (size < 0) {
    return Status::IoError;
  }
  out.resize(static_cast<std::size_t>(size));
  file.seekg(0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): std::byte storage read as chars
  if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
    return Status::IoError;
  }
  return Status::Ok;
}

std::vector<SnapshotEntry> list_snapshots(const std::string& directory) {
  std::vector<SnapshotEntry> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix)) {
      continue;
    }
    const std::string_view digits{name.data() + kPrefix.size(),
                                  name.size() - kPrefix.size() - kSuffix.size()};
    std::uint64_t seq = 0;
    const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), seq);
    if (err == std::errc{} && end == digits.data() + digits.size()) {
      out.push_back(SnapshotEntry{seq, entry.path().string()});
    }
  }
  std::sort(out.begin(), out.end(),
            [](const SnapshotEntry& a, const SnapshotEntry& b) { return a.seq < b.seq; });
  return out;
}

Status load_snapshot(const std::string& path, LoadedSnapshot& out, std::string& error) {
  Status s = read_file_bytes(path, out.file);
  std::span<const std::byte> body;
  if (core::ok(s)) {
    s = decode_snapshot(out.file, out.header, out.info, body);
  }
  if (!core::ok(s)) {
    error = path + ": cannot read the snapshot: " + std::string{core::to_string(s)};
    return s;
  }
  out.body_offset = static_cast<std::size_t>(body.data() - out.file.data());
  out.body_size = body.size();
  if (!out.info.complete) {
    error = path + ": the snapshot lacks the state of a strategy that does not describe it";
    return Status::InvalidState;
  }
  const std::string_view commit = build_info().git_commit;
  using Commit = decltype(out.header.git_commit);
  if (out.header.git_commit.view() != commit.substr(0, Commit::capacity())) {
    error = path + ": the snapshot was written by build " +
            std::string{out.header.git_commit.view()} + " and this is " + std::string{commit} +
            "; a state is only restored by the build that wrote it";
    return Status::InvalidState;
  }
  return Status::Ok;
}

bool pick_snapshot(const std::string& directory, std::uint64_t min_seq, SnapshotPick pick,
                   SnapshotEntry& out) {
  std::vector<SnapshotEntry> all = list_snapshots(directory);
  if (pick == SnapshotPick::Latest) {
    std::reverse(all.begin(), all.end());
  }
  for (const SnapshotEntry& entry : all) {
    if (entry.seq < min_seq) {
      continue;
    }
    LoadedSnapshot loaded;
    std::string error;
    if (core::ok(load_snapshot(entry.path, loaded, error))) {
      out = entry;
      return true;
    }
  }
  return false;
}

} // namespace jarvis::node
