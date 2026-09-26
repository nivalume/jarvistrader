#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"

// Backtest inputs from the data catalog (docs/architecture.md section 16.1). A catalog holds one
// decoded event log per instrument, stream and UTC day:
//
//   {catalog}/{instrument_id}/{stream}/{YYYY-MM-DD}/events-NNNNNN.jlog
//
// written by the converters in `jarvis.data` (data.binance.vision, nautilus Parquet). A
// LogSource reads the days of one stream in order; the node merges the streams by key.

namespace jarvis::node {

// Yields the input records of several logs, one directory after another; output records are
// skipped. An event that borrows storage (OrderBookDeltas) stays valid until the next call.
// Each log numbers its records from 1, so a later directory's seq is offset by the records
// already read: keys stay strictly increasing across days even when two days meet at the same
// timestamp.
class LogSource {
public:
  LogSource() = default;
  LogSource(const LogSource&) = delete;
  LogSource& operator=(const LogSource&) = delete;
  LogSource(LogSource&&) = default;
  LogSource& operator=(LogSource&&) = default;
  ~LogSource() = default;

  [[nodiscard]] core::Status open(std::vector<std::string> directories);
  [[nodiscard]] core::Status next(core::EventKey& key, model::Event& event);

  [[nodiscard]] const std::string& directory() const noexcept;

private:
  std::vector<std::string> directories_;
  std::size_t index_ = 0;
  std::uint64_t seq_offset_ = 0; // records of the directories before index_
  std::uint64_t last_seq_ = 0;   // seq of the last record read, before the offset
  EventLogReader reader_;
  bool opened_ = false;
};

struct CatalogStream {
  model::InstrumentId instrument;
  std::string stream;
  std::vector<std::string> days; // log directories, oldest first
};

// The catalog directories that [data] selects: every configured (instrument, stream) with the
// days that overlap data.range (all days when there is no range). A stream without any day in
// range is an error, reported in `error` with the path that was searched.
[[nodiscard]] core::Status resolve_catalog(const DataSection& data, std::vector<CatalogStream>& out,
                                           std::string& error);

// {catalog}/{instrument_id}/{stream}/{day}
[[nodiscard]] std::string catalog_directory(const std::string& catalog,
                                            const model::InstrumentId& instrument,
                                            const std::string& stream, const std::string& day);

} // namespace jarvis::node
