#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/model/wire.hpp"

// EngineState snapshot files (docs/architecture.md section 16.3): snapshot-<seq>.jsnap in the run
// directory, one per snapshot point, written whole or not at all (a temporary file, renamed).
//
//   magic "JARVISSN" | format u16 | the run's log header (model/wire.hpp) | seq u64 | ts u64 |
//   complete u8 | strategies u16 | body_len u64 | body | crc32c u32 (over everything before)
//
// `seq` and `ts` are those of the input after which the state was taken (a BatchEnd); the body
// is Engine::save_state. `complete` says every strategy's own state is in it, so restoring it
// continues exactly (strategy.hpp).

namespace jarvis::node {

inline constexpr std::uint16_t kSnapshotFormat = 1;

struct SnapshotInfo {
  std::uint64_t seq = 0;
  std::uint64_t ts = 0;
  bool complete = false;
  std::uint16_t strategies = 0;
};

[[nodiscard]] std::string snapshot_name(std::uint64_t seq);

[[nodiscard]] core::Status encode_snapshot(const model::wire::LogHeader& header,
                                           const SnapshotInfo& info,
                                           std::span<const std::byte> body,
                                           std::vector<std::byte>& out);
// `body` points into `file`.
[[nodiscard]] core::Status decode_snapshot(std::span<const std::byte> file,
                                           model::wire::LogHeader& header, SnapshotInfo& info,
                                           std::span<const std::byte>& body);

// Writes the encoded snapshot as `directory`/snapshot_name(seq): a temporary file first, then a
// rename. With `durable` the file and the directory are synced.
[[nodiscard]] core::Status write_snapshot_file(const std::string& directory, std::uint64_t seq,
                                               std::span<const std::byte> encoded, bool durable);
[[nodiscard]] core::Status read_file_bytes(const std::string& path, std::vector<std::byte>& out);

struct SnapshotEntry {
  std::uint64_t seq = 0;
  std::string path;
};
// The snapshot files of `directory`, by seq.
[[nodiscard]] std::vector<SnapshotEntry> list_snapshots(const std::string& directory);

// A snapshot file read whole and decoded.
struct LoadedSnapshot {
  model::wire::LogHeader header;
  SnapshotInfo info;
  std::vector<std::byte> file;
  std::size_t body_offset = 0;
  std::size_t body_size = 0;
  [[nodiscard]] std::span<const std::byte> body() const noexcept {
    return std::span<const std::byte>{file}.subspan(body_offset, body_size);
  }
};

// Reads the snapshot file `path` for a restore. It must decode, be complete (every strategy's
// state in it), and come from this build's commit: the state layout is the build's, so a
// snapshot another build wrote is not restored. `error` says which of these failed.
[[nodiscard]] core::Status load_snapshot(const std::string& path, LoadedSnapshot& out,
                                         std::string& error);

enum class SnapshotPick : std::uint8_t { Earliest, Latest };

// The earliest (or latest) snapshot of `directory` with seq >= `min_seq` that load_snapshot
// accepts; false when there is none.
[[nodiscard]] bool pick_snapshot(const std::string& directory, std::uint64_t min_seq,
                                 SnapshotPick pick, SnapshotEntry& out);

} // namespace jarvis::node
