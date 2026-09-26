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

namespace jarvis::node {

// Event log directory: segments named events-000000.jlog, events-000001.jlog, ... Each segment
// starts with a LogHeader; records follow back to back (docs/architecture.md section 16.1).

struct EventLogOptions {
  std::uint64_t segment_bytes = 256ULL << 20U; // roll to a new segment beyond this size
  bool sync_on_flush = false;                  // fdatasync after each flush (persistence=barrier)
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
  [[nodiscard]] core::Status flush();
  [[nodiscard]] core::Status close();

  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }

private:
  [[nodiscard]] core::Status open_segment();

  int fd_ = -1;
  std::string directory_;
  model::wire::LogHeader header_;
  EventLogOptions options_;
  std::uint64_t segment_size_ = 0;
  std::uint64_t records_ = 0;
  std::vector<std::byte> buffer_;
  std::vector<std::byte> scratch_;
};

class EventLogReader {
public:
  EventLogReader();

  [[nodiscard]] core::Status open(const std::string& directory);
  // Header of the first segment; later segments must agree on everything but the index.
  [[nodiscard]] const model::wire::LogHeader& header() const noexcept { return header_; }
  // The next record in log order, or Status::EndOfStream. The view is valid until the next call.
  [[nodiscard]] core::Status next(model::wire::RecordView& out);
  // Decodes the record most recently returned by next().
  [[nodiscard]] core::Status decode(const model::wire::RecordView& record, model::Event& out);
  // Decodes an output record (kind >= model::wire::kFirstOutputKind).
  [[nodiscard]] static core::Status decode_output(const model::wire::RecordView& record,
                                                  model::Output& out);

private:
  [[nodiscard]] core::Status load_segment(std::size_t index);

  std::vector<std::string> segments_;
  std::size_t segment_ = 0;
  std::vector<std::byte> data_;
  std::size_t pos_ = 0;
  model::wire::LogHeader header_;
  model::wire::DecodeScratch scratch_;
};

[[nodiscard]] std::string segment_name(std::uint32_t index);

} // namespace jarvis::node
