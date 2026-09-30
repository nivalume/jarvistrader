#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/spsc_ring.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/network/io.hpp"

// The md-io thread (docs/architecture.md sections 7.1, 13 and 14): it owns the market data
// connections, decodes every message in place, keeps the order books in sync (snapshots from
// the WebSocket API), and hands the core normalised events as wire records through one byte
// ring. Every message is also written to the raw frame file when one is configured, the
// WebSocket API answers included, so that the decoded stream can be rebuilt from it.
//
// A dropped connection reconnects with backoff; the depth books leave Synced when the depth
// connection drops (the kernel gets a lone CLEAR) and sync again from a fresh snapshot. The feed
// records ConnectionStatus(MarketData) up once every stream connection is open and down when one
// closes; the sync gate moves a Running node to Degraded while it is down. A full
// ring stops the thread until the core has read (back-pressure: dropping a book diff would
// desynchronise the book).

namespace jarvis::live {

struct FeedEndpoints {
  std::string streams = "wss://fstream.binance.com"; // /public and /market routes appended
  std::string ws_api = "wss://ws-fapi.binance.com/ws-fapi/v1";
  network::TlsOptions tls;
};

struct MarketFeedConfig {
  FeedEndpoints endpoints;
  adapter::SymbolTable symbols;     // the instruments, in the order of `symbol_names`
  std::vector<std::string> names;   // venue symbols ("BTCUSDT") by table index
  std::vector<std::string> streams; // full stream names ("btcusdt@aggTrade")
  model::Venue venue;               // of the ConnectionStatus(MarketData) records
  std::string raw_frames;           // raw frame file; empty: none
  std::size_t ring_bytes = std::size_t{8} << 20U;
  std::uint16_t source_id = 1;
  int snapshot_limit = 1000;
  std::size_t max_levels = 2000; // per book side
  std::chrono::milliseconds reconnect_initial{500};
  std::chrono::milliseconds reconnect_max{30'000};
  bool busy_poll = false; // the thread never sleeps: it polls its connections in a loop
  std::vector<int> cpus;  // the CPUs the thread runs on (cpu_affinity.hpp); empty: any
};

struct MarketFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t events = 0;
  std::uint64_t decode_errors = 0;
  std::uint64_t unsupported = 0;
  std::uint64_t ring_waits = 0; // times the ring was full
  std::uint64_t connects = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t snapshot_failures = 0;
  std::uint64_t book_syncs = 0;
};

class MarketFeed {
public:
  MarketFeed(const ArrivalClock& clock, MarketFeedConfig config);
  ~MarketFeed();
  MarketFeed(const MarketFeed&) = delete;
  MarketFeed& operator=(const MarketFeed&) = delete;

  // Opens the raw frame file and starts the thread.
  [[nodiscard]] core::Status start(std::string& error);
  void stop(); // closes the connections and joins the thread

  // The core's side: records written by encode_record.
  [[nodiscard]] SpscByteRing& ring() noexcept;
  [[nodiscard]] MarketFeedStats stats() const noexcept;
  [[nodiscard]] std::uint16_t source_id() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace jarvis::live
