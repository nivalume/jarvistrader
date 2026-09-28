#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/node/event_log.hpp"

// Rebuilding decoded market data from a raw frame file (docs/architecture.md section 13.4): the
// same codec and depth sync as the md-io thread, over the same frames in the same order, with
// each WebSocket API snapshot answer paired with its symbol by the recorded request. Since the
// feed thread writes every frame in the order it processes them, the result is the event
// stream the feed handed the core, event for event.

namespace jarvis::live {

struct RedecodedEvent {
  std::uint16_t kind = 0;         // the wire record kind
  std::vector<std::byte> payload; // the wire payload
};

struct RedecodeResult {
  std::vector<RedecodedEvent> events;
  std::uint64_t frames = 0;
  std::uint64_t messages = 0; // stream messages decoded
  std::uint64_t decode_errors = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t book_syncs = 0;
};

// Decodes `raw_path` with the instruments of `table`; with `writer`, also appends every event
// (keyed by arrival time, source 1) to that event log.
[[nodiscard]] core::Status redecode(const std::string& raw_path, const adapter::SymbolTable& table,
                                    node::EventLogWriter* writer, RedecodeResult& out,
                                    std::string& error);

// The market data inputs of a run (the feed's source id, 1; its ConnectionStatus records aside,
// since they come from connections, not frames) must be exactly the first
// redecoded events, in order and byte for byte; later events reached the feed after the run
// stopped. Ok with `compared` set, or InvalidState with `error` naming the first difference.
[[nodiscard]] core::Status check_market_inputs(const std::string& run_dir,
                                               const std::vector<RedecodedEvent>& events,
                                               std::size_t& compared, std::string& error);

} // namespace jarvis::live
