#include "jarvis/node/epoch_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include "jarvis/model/client_order_id.hpp"

namespace jarvis::node {

using core::Status;

namespace {

constexpr std::string_view kMagic = "jarvis-epoch v1\n";

Status write_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IoError;
    }
    bytes.remove_prefix(static_cast<std::size_t>(n));
  }
  return Status::Ok;
}

Status sync_directory(const std::filesystem::path& dir) {
  const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return Status::IoError;
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  return rc == 0 ? Status::Ok : Status::IoError;
}

Status write_epoch(const std::string& path, std::uint64_t epoch) {
  const std::filesystem::path target{path};
  const std::string temp = path + ".tmp";
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return Status::IoError;
  }
  const std::string content = std::string{kMagic} + std::to_string(epoch) + "\n";
  Status s = write_all(fd, content);
  if (core::ok(s) && ::fsync(fd) != 0) {
    s = Status::IoError;
  }
  ::close(fd);
  if (!core::ok(s)) {
    return s;
  }
  std::error_code ec;
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    return Status::IoError;
  }
  return sync_directory(target.parent_path());
}

} // namespace

Status read_epoch(const std::string& path, std::uint64_t& epoch) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return ec ? Status::IoError : Status::NotFound;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return Status::IoError;
  }
  const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  if (!text.starts_with(kMagic) || !text.ends_with('\n')) {
    return Status::ParseError;
  }
  const std::string_view digits =
      std::string_view{text}.substr(kMagic.size(), text.size() - kMagic.size() - 1);
  std::uint64_t value = 0;
  const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (err != std::errc{} || end != digits.data() + digits.size() || digits.empty()) {
    return Status::ParseError;
  }
  if (value == 0 || value > model::kMaxEpoch) {
    return Status::OutOfRange;
  }
  epoch = value;
  return Status::Ok;
}

Status next_epoch(const std::string& path, std::uint64_t& epoch) {
  std::uint64_t last = 0;
  const Status s = read_epoch(path, last);
  if (!core::ok(s) && s != Status::NotFound) {
    return s;
  }
  if (last >= model::kMaxEpoch) {
    return Status::Overflow;
  }
  const Status w = write_epoch(path, last + 1);
  if (!core::ok(w)) {
    return w;
  }
  epoch = last + 1;
  return Status::Ok;
}

} // namespace jarvis::node
