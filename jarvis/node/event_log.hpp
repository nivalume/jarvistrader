#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/snapshot_file.hpp"

namespace jarvis::node {

// Event log directory: segments named events-000000.jlog, events-000001.jlog, ... Each segment
// starts with a LogHeader; records follow back to back (docs/architecture.md section 16.1). A log
// whose older segments were removed after a snapshot (persistence.truncate) starts at a later
// index; its segments are still consecutive.

struct EventLogOptions {
  std::uint64_t segment_bytes = 256ULL << 20U; // roll to a new segment beyond this size
  bool sync_on_flush = false;                  // fdatasync after each flush
  // A finished segment is fdatasync'ed before it is closed, and the directory after a segment is
  // created, so that what sync() made durable stays reachable after a crash (the persist thread).
  bool durable = false;
};

class EventLogWriter {
public:
  EventLogWriter() = default;
  EventLogWriter(const EventLogWriter&) = delete;
  EventLogWriter& operator=(const EventLogWriter&) = delete;
  EventLogWriter(EventLogWriter&& other) noexcept;
  EventLogWriter& operator=(EventLogWriter&& other) noexcept;
  ~EventLogWriter();

  // Creates `directory` if needed; refuses to overwrite an existing log.
  [[nodiscard]] core::Status open(const std::string& directory,
                                  const model::wire::LogHeader& header,
                                  EventLogOptions options = {});
  [[nodiscard]] core::Status append(const core::EventKey& key, const model::Event& event);
  // Appends a kernel output; `key` is the causing input's key with source_id = output index.
  [[nodiscard]] core::Status append_output(const core::EventKey& key, const model::Output& output);
  // Appends an already encoded record (as produced by model::wire::encode_record).
  [[nodiscard]] core::Status append_record(std::span<const std::byte> record);
  // Writes an EngineState snapshot file beside the segments (snapshot_file.hpp); synced with
  // `durable`.
  [[nodiscard]] core::Status write_snapshot(const SnapshotInfo& info,
                                            std::span<const std::byte> body);
  [[nodiscard]] core::Status flush();
  // Removes the finished segments that hold only records with seq <= `seq` (the seq of a complete
  // snapshot already written), oldest first, then the snapshot files older than the first record
  // left, which nothing can replay from any more. `removed` counts the segments.
  [[nodiscard]] core::Status truncate_before(std::uint64_t seq, std::uint32_t& removed);
  // flush() and fdatasync: every record appended so far survives a crash.
  [[nodiscard]] core::Status sync();
  [[nodiscard]] core::Status close();

  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
  // Bytes of records appended so far, across segments (segment headers not counted): the log
  // position after the latest record.
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
  [[nodiscard]] std::uint32_t segment_index() const noexcept { return header_.segment_index; }

private:
  [[nodiscard]] core::Status open_segment();
  [[nodiscard]] core::Status sync_fd() const;

  int fd_ = -1;
  std::string directory_;
  model::wire::LogHeader header_;
  std::vector<std::uint64_t> first_seqs_; // per segment written: the seq of its first record
  std::uint32_t first_segment_ = 0;       // the oldest segment not removed
  EventLogOptions options_;
  std::uint64_t segment_size_ = 0;
  std::uint64_t records_ = 0;
  std::uint64_t position_ = 0;
  std::vector<std::byte> buffer_;
  std::vector<std::byte> scratch_;
};

struct EventLogReadOptions {
  // A log a crash interrupted ends in a partial record (or a segment header cut short). With this
  // set, the first record of the last segment that does not decode (short, or failing its CRC)
  // ends the log instead of failing it; torn_bytes() tells how much was left out. Anything
  // wrong in an earlier segment is still an error. A log whose only segment has a torn header
  // holds no records (and header() is the default).
  bool tolerate_torn_tail = false;
};

class EventLogReader {
public:
  EventLogReader();

  [[nodiscard]] core::Status open(const std::string& directory, EventLogReadOptions options = {});
  // Header of the first segment; later segments must agree on everything but the index, which
  // counts up from the first segment's.
  [[nodiscard]] const model::wire::LogHeader& header() const noexcept { return header_; }
  // The next record in log order, or Status::EndOfStream. The view is valid until the next call.
  [[nodiscard]] core::Status next(model::wire::RecordView& out);
  // Decodes the record most recently returned by next().
  [[nodiscard]] core::Status decode(const model::wire::RecordView& record, model::Event& out);
  // Decodes an output record (kind >= model::wire::kFirstOutputKind).
  [[nodiscard]] static core::Status decode_output(const model::wire::RecordView& record,
                                                  model::Output& out);

  // With tolerate_torn_tail, once next() has returned EndOfStream: the bytes after the last
  // whole record of the last segment (0 for a clean end), and that segment's path.
  [[nodiscard]] std::uint64_t torn_bytes() const noexcept { return torn_bytes_; }
  [[nodiscard]] const std::string& torn_segment() const noexcept { return torn_segment_; }
  // The file offset just past the last record next() returned, in the current segment.
  [[nodiscard]] std::size_t segment_offset() const noexcept { return pos_; }
  [[nodiscard]] const std::vector<std::string>& segments() const noexcept { return segments_; }

private:
  [[nodiscard]] core::Status load_segment(std::size_t index);
  [[nodiscard]] bool last_segment() const noexcept { return segment_ + 1 >= segments_.size(); }
  core::Status torn(std::size_t from);

  EventLogReadOptions options_;
  std::vector<std::string> segments_;
  std::size_t segment_ = 0;
  std::vector<std::byte> data_;
  std::size_t pos_ = 0;
  model::wire::LogHeader header_;
  model::wire::DecodeScratch scratch_;
  std::uint64_t torn_bytes_ = 0;
  std::string torn_segment_;
};

[[nodiscard]] std::string segment_name(std::uint32_t index);

} // namespace jarvis::node
