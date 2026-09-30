#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/wire.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/event_log.hpp"

// The persist thread (docs/architecture.md sections 7.1 and 16.2). In sandbox and live the core
// thread does not write the event log itself: it encodes each record and hands it over a SPSC
// byte ring, and the persist thread appends the records to the segments and makes them durable
// (fdatasync). The log's bytes are the same as those EventLogWriter writes on the core thread.
//
// The log position is the byte count of the records appended so far (segment headers not
// counted). durable() is the position up to which the log is known to survive a crash: the
// persist thread publishes it after each fdatasync.
//
//   async    the persist thread syncs at most every `sync_every`; the core never waits for it;
//   barrier  it syncs after every batch it takes from the ring, and the venue-io thread sends a
//            command only once durable() has passed the command's record (live_node.hpp), so a
//            command that reached the venue is always in the local log.
//
// A full ring makes the core wait (counted in stalls); a record is never dropped. When a write
// or a sync fails the persist thread stops taking records and durable() stops moving: the core's
// next append fails with IoError, and in barrier mode no further command leaves.

namespace jarvis::live {

struct PersistConfig {
  node::EventLogOptions log; // `durable` is always set
  std::size_t ring_bytes = std::size_t{64} << 20U;
  bool barrier = false;
  std::chrono::milliseconds sync_every{100}; // async; 0: after every batch, as barrier does
};

struct PersistStats {
  std::uint64_t records = 0; // appended by the core
  std::uint64_t position = 0;
  std::uint64_t durable = 0;
  std::uint64_t syncs = 0;
  std::uint64_t stalls = 0;   // appends that found the ring full
  std::uint64_t max_lag = 0;  // most bytes one sync made durable: the widest window at risk
  std::uint64_t segments = 0; // segments written
};

// From [persistence]: barrier for mode = "barrier", sync_every_ms.
[[nodiscard]] PersistConfig persist_config(const node::NodeConfig& config);

class Persister {
public:
  explicit Persister(const PersistConfig& config);
  ~Persister();
  Persister(const Persister&) = delete;
  Persister& operator=(const Persister&) = delete;
  Persister(Persister&&) = delete;
  Persister& operator=(Persister&&) = delete;

  // Opens the log (EventLogWriter::open) and starts the persist thread.
  [[nodiscard]] core::Status open(const std::string& directory,
                                  const model::wire::LogHeader& header, std::string& error);

  // The core thread.
  [[nodiscard]] core::Status append(const core::EventKey& key, const model::Event& event);
  [[nodiscard]] core::Status append_output(const core::EventKey& key, const model::Output& output);
  [[nodiscard]] core::Status append_record(std::span<const std::byte> record);
  // The log position after the latest record the core appended.
  [[nodiscard]] std::uint64_t position() const noexcept;

  // Any thread.
  [[nodiscard]] const std::atomic<std::uint64_t>& durable() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] PersistStats stats() const noexcept;

  // Takes the rest of the ring, syncs, closes the log and joins the thread. The first error the
  // persist thread met, if any, else Ok.
  [[nodiscard]] core::Status close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace jarvis::live
