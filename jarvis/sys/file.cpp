#include "jarvis/sys/file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace jarvis::sys {

using core::Status;

File::~File() { static_cast<void>(close()); }

File::File(File&& other) noexcept {
#if defined(_WIN32)
  handle_ = std::exchange(other.handle_, nullptr);
#else
  fd_ = std::exchange(other.fd_, -1);
#endif
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
#if defined(_WIN32)
    handle_ = std::exchange(other.handle_, nullptr);
#else
    fd_ = std::exchange(other.fd_, -1);
#endif
  }
  return *this;
}

Status File::write_all(std::string_view text) {
  return write_all(std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

#if defined(_WIN32)

Status File::open(const std::string& path, Mode mode) {
  if (handle_ != nullptr) {
    return Status::InvalidState;
  }
  const std::filesystem::path native{path};
  // Readers may open the file while it is written, and the log's old segments may be removed
  // (persistence.truncate) by name.
  HANDLE h = ::CreateFileW(native.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           mode == Mode::CreateNew ? CREATE_NEW : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? Status::AlreadyExists
                                                                       : Status::IoError;
  }
  handle_ = h;
  return Status::Ok;
}

Status File::write_all(std::span<const std::byte> bytes) {
  if (handle_ == nullptr) {
    return Status::InvalidState;
  }
  constexpr std::size_t kChunk = std::size_t{1} << 30U;
  while (!bytes.empty()) {
    const auto n = static_cast<DWORD>(std::min(bytes.size(), kChunk));
    DWORD written = 0;
    if (::WriteFile(handle_, bytes.data(), n, &written, nullptr) == 0 || written == 0) {
      return Status::IoError;
    }
    bytes = bytes.subspan(written);
  }
  return Status::Ok;
}

Status File::sync_data() { return sync(); }

Status File::sync() {
  if (handle_ == nullptr) {
    return Status::InvalidState;
  }
  return ::FlushFileBuffers(handle_) != 0 ? Status::Ok : Status::IoError;
}

Status File::close() {
  if (handle_ == nullptr) {
    return Status::Ok;
  }
  const bool closed = ::CloseHandle(handle_) != 0;
  handle_ = nullptr;
  return closed ? Status::Ok : Status::IoError;
}

bool File::is_open() const noexcept { return handle_ != nullptr; }

Status sync_directory(const std::string& /*directory*/) {
  return Status::Ok; // NTFS journals directory entries; replace_file writes through
}

Status replace_file(const std::string& from, const std::string& to) {
  const std::filesystem::path source{from};
  const std::filesystem::path target{to};
  return ::MoveFileExW(source.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0
             ? Status::Ok
             : Status::IoError;
}

#else

Status File::open(const std::string& path, Mode mode) {
  if (fd_ >= 0) {
    return Status::InvalidState;
  }
  const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (mode == Mode::CreateNew ? O_EXCL : O_TRUNC);
  const int fd = ::open(path.c_str(), flags, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (fd < 0) {
    return errno == EEXIST ? Status::AlreadyExists : Status::IoError;
  }
  fd_ = fd;
  return Status::Ok;
}

// Writing and syncing change the file, so they are not const although they only read fd_.
// NOLINTBEGIN(readability-make-member-function-const)
Status File::write_all(std::span<const std::byte> bytes) {
  if (fd_ < 0) {
    return Status::InvalidState;
  }
  while (!bytes.empty()) {
    const ssize_t n = ::write(fd_, bytes.data(), bytes.size());
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

Status File::sync_data() {
  if (fd_ < 0) {
    return Status::InvalidState;
  }
#if defined(__APPLE__)
  return ::fsync(fd_) == 0 ? Status::Ok : Status::IoError;
#else
  return ::fdatasync(fd_) == 0 ? Status::Ok : Status::IoError;
#endif
}

Status File::sync() {
  if (fd_ < 0) {
    return Status::InvalidState;
  }
  return ::fsync(fd_) == 0 ? Status::Ok : Status::IoError;
}
// NOLINTEND(readability-make-member-function-const)

Status File::close() {
  if (fd_ < 0) {
    return Status::Ok;
  }
  const bool closed = ::close(fd_) == 0;
  fd_ = -1;
  return closed ? Status::Ok : Status::IoError;
}

bool File::is_open() const noexcept { return fd_ >= 0; }

Status sync_directory(const std::string& directory) {
  const char* path = directory.empty() ? "." : directory.c_str();
  const int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); // NOLINT(*-vararg)
  if (fd < 0) {
    return Status::IoError;
  }
  const Status s = ::fsync(fd) == 0 ? Status::Ok : Status::IoError;
  ::close(fd);
  return s;
}

Status replace_file(const std::string& from, const std::string& to) {
  return ::rename(from.c_str(), to.c_str()) == 0 ? Status::Ok : Status::IoError;
}

#endif

} // namespace jarvis::sys
