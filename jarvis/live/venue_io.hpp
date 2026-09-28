#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>

#include "jarvis/adapter/binance/order_tracker.hpp"
#include "jarvis/adapter/binance/user_stream_session.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/live/raw_frames.hpp"
#include "jarvis/live/spsc_ring.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/network/io.hpp"
#include "jarvis/network/signer.hpp"

// The venue-io thread (docs/architecture.md sections 7.1, 14.4, 14.5 and 15): the account's
// side of the venue. One IO thread owns the WebSocket API (order entry) and the user data stream,
// and with them the OrderTracker that turns both into kernel events, so the tracker has one
// writer. The core hands it commands through a SPSC ring; it hands the core, through one byte
// ring and in the order they happened, ConnectionStatus, order events, AccountState,
// RateLimitFeedback and VenueSnapshot records.
//
// A second thread does what blocks: the listenKey (created at start, kept alive, renewed when the
// venue says it expired) and the reconciliation snapshot over REST (adapter/binance/snapshot.hpp).
// When the user stream is live the IO thread records ConnectionStatus(up) and asks the REST thread
// for a snapshot; the answer is recorded only while the stream is still live on that connection
// (a drop in between makes it stale), and a failed snapshot is asked for again. A dropped stream
// records ConnectionStatus(down): the kernel halts trading and holds venue events until the next
// snapshot. Order entry going up or down is recorded as ConnectionStatus(OrderEntry).
//
// Commands while the WebSocket API is down are refused locally (OrderRejected, OrderModifyRejected,
// OrderCancelRejected with reason "BINANCE_0 order entry is down"): nothing reached the venue. An
// order whose outcome is unknown (a timeout, a connection lost with it in flight) is left for
// reconciliation.
//
// The IO thread polls the command ring between rounds of network work: with busy_poll it never
// sleeps; otherwise a round lasts at most a millisecond.

namespace jarvis::live {

struct VenueEndpoints {
  std::string rest = "https://fapi.binance.com";
  std::string ws_api = "wss://ws-fapi.binance.com/ws-fapi/v1";
  std::string user_stream = "wss://fstream.binance.com/private/stream";
  network::TlsOptions tls;
};

// A command from the core for the venue.
using VenueCommand = std::variant<model::SubmitOrder, model::ModifyOrder, model::CancelOrder>;

struct VenueIoConfig {
  VenueEndpoints endpoints;
  std::string api_key;
  network::Signer signer; // HMAC or Ed25519 (the WebSocket API then logs on)
  adapter::SymbolTable symbols;
  adapter::binance::VenueIdentity identity;
  model::Venue venue;               // of the ConnectionStatus records
  std::int64_t time_offset_ms = 0;  // the venue's clock minus the local one (startup checks)
  std::int64_t trades_since_ms = 0; // the node's start: earlier trades belong to no order it knows
  std::string raw_frames;           // user stream frames and WebSocket API answers; empty: none
  std::size_t ring_bytes = std::size_t{16} << 20U; // a snapshot record can take 1 MiB
  std::size_t command_slots = 4096;
  std::uint16_t source_id = 2;
  bool busy_poll = false;
  std::chrono::milliseconds snapshot_retry{2'000};
  std::chrono::milliseconds reconnect_initial{500};
  std::chrono::milliseconds reconnect_max{30'000};
  std::chrono::milliseconds rest_tick{1'000}; // the REST thread's listenKey check
  adapter::binance::ListenKeyKeeperConfig keeper;
  std::function<std::int64_t()> now_ms; // local UTC milliseconds for signing; system clock if empty
};

struct VenueIoStats {
  std::uint64_t events = 0;          // records written to the ring
  std::uint64_t ring_waits = 0;      // times the ring was full
  std::uint64_t commands = 0;        // taken from the command ring
  std::uint64_t refused_locally = 0; // commands refused while order entry was down
  std::uint64_t unknown_outcomes = 0;
  std::uint64_t frames = 0; // user stream frames
  std::uint64_t decode_errors = 0;
  std::uint64_t snapshots = 0;         // recorded
  std::uint64_t snapshot_failures = 0; // asked for again
  std::uint64_t stale_snapshots = 0;   // the stream dropped before the answer
  std::uint64_t key_failures = 0;      // listenKey calls that failed
};

class VenueIo {
public:
  VenueIo(const ArrivalClock& clock, VenueIoConfig config);
  ~VenueIo();
  VenueIo(const VenueIo&) = delete;
  VenueIo& operator=(const VenueIo&) = delete;

  // Opens the raw frame file and starts both threads.
  [[nodiscard]] core::Status start(std::string& error);
  void stop(); // closes the connections and joins the threads

  [[nodiscard]] SpscByteRing& ring() noexcept;               // venue -> core
  [[nodiscard]] SpscRing<VenueCommand>& commands() noexcept; // core -> venue
  [[nodiscard]] VenueIoStats stats() const noexcept;
  [[nodiscard]] std::uint16_t source_id() const noexcept;
  // The last error of the REST thread (listenKey, snapshot), for the operator.
  [[nodiscard]] std::string last_error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace jarvis::live
