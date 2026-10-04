#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "jarvis/core/status.hpp"

// Files written to survive a crash (the event log, snapshots, the epoch file;
// docs/architecture.md section 16): written through the operating system's own handle so that
// they can be synced, and replaced atomically.
//
//   Linux    open(2), fdatasync(2), fsync(2) of the directory, rename(2)
//   macOS    the same, with fsync(2) for the data
//   Windows  CreateFileW, FlushFileBuffers, MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH); a
//            directory needs no sync (NTFS journals its entries)
//
// Paths are narrow strings interpreted as std::filesystem::path does (the active code page on
// Windows), so that the same string names the same file here and in <fstream>.

namespace jarvis::sys {

class File {
public:
  enum class Mode : unsigned char {
    CreateNew, // fails with AlreadyExists if the file exists
    Truncate,  // creates it, or empties it
  };

  File() = default;
  ~File();
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  // Opens `path` for writing. IoError (or AlreadyExists for CreateNew) on failure.
  [[nodiscard]] core::Status open(const std::string& path, Mode mode);
  [[nodiscard]] core::Status write_all(std::span<const std::byte> bytes);
  [[nodiscard]] core::Status write_all(std::string_view text);
  // The data and what is needed to read it back (fdatasync); sync() also flushes the rest of
  // the file's metadata (fsync).
  [[nodiscard]] core::Status sync_data();
  [[nodiscard]] core::Status sync();
  // Closes the file; Ok when it was not open.
  core::Status close();
  [[nodiscard]] bool is_open() const noexcept;

private:
#if defined(_WIN32)
  void* handle_ = nullptr; // HANDLE; nullptr when closed
#else
  int fd_ = -1;
#endif
};

// Makes the entries created or renamed in `directory` survive a crash ("" is the current
// directory).
[[nodiscard]] core::Status sync_directory(const std::string& directory);

// Replaces `to` with `from` in one step: a reader sees the old file or the new one, never a mix.
[[nodiscard]] core::Status replace_file(const std::string& from, const std::string& to);

// Whether files carry POSIX permission bits that keep them to their owner. Windows files carry
// access control lists instead, which the node does not inspect.
#if defined(_WIN32)
inline constexpr bool kPermissionBits = false;
#else
inline constexpr bool kPermissionBits = true;
#endif

} // namespace jarvis::sys
