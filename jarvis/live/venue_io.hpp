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
// venue says it expired), the reconciliation snapshot over REST (adapter/binance/snapshot.hpp)
// and the countdownCancelAll calls of the dead man's switch (a failure is counted and reported in
// last_error; the countdown still armed at the venue keeps running). Every `check_every` while
// the stream's current connection has been reconciled it also reads the light check (open orders
// and positions, section 15.3), recorded as a VenueSnapshot with `check` set; one that fails or
// outlives its connection is dropped, and the next tick tries again.
// When the user stream is live the IO thread records ConnectionStatus(up) and asks the REST thread
// for a snapshot; the answer is recorded only while the stream is still live on that connection
// (a drop in between makes it stale), and a failed snapshot is asked for again. A dropped stream
// records ConnectionStatus(down): the kernel halts trading and holds venue events until the next
// snapshot. Order entry going up or down is recorded as ConnectionStatus(OrderEntry).
//
// Commands while the WebSocket API is down go over REST on the REST thread (rest_fallback), with
// the same outcomes: acknowledged, refused with the venue's code, or unknown (a 5xx, a timeout).
// Without the fallback they are refused locally (OrderRejected, OrderModifyRejected,
// OrderCancelRejected with reason "BINANCE_0 order entry is down"): nothing reached the venue. An
// order whose outcome is unknown (a timeout, a connection lost with it in flight) is left for
// reconciliation.
//
// The IO thread polls the command ring between rounds of network work: with busy_poll it never
// sleeps; otherwise a round lasts at most a millisecond.
//
// persistence.mode = "barrier" (section 16.2): each queued command carries the event log position
// just past its own record, and the IO thread sends it only once the persist thread's durable
// position (VenueIoConfig::durable) has reached that. Commands keep their order: one waiting at
// the head of the ring holds the ones behind it.

namespace jarvis::live {

struct VenueEndpoints {
  std::string rest = "https://fapi.binance.com";
  std::string ws_api = "wss://ws-fapi.binance.com/ws-fapi/v1";
  std::string user_stream = "wss://fstream.binance.com/private/stream";
  network::TlsOptions tls;
};

// A command from the core for the venue.
using VenueCommand = std::variant<model::SubmitOrder, model::ModifyOrder, model::CancelOrder,
                                  model::CountdownCancelAll>;

// A command in the ring, with the log position it waits for (0: none).
struct QueuedCommand {
  QueuedCommand() = default;
  // NOLINTNEXTLINE(google-explicit-constructor): a command with no barrier
  QueuedCommand(const VenueCommand& c, std::uint64_t durable = 0)
      : command{c}, durable_at{durable} {}
  VenueCommand command;
  std::uint64_t durable_at = 0;
};

struct VenueIoConfig {
  VenueEndpoints endpoints;
  std::string api_key;
  network::Signer signer; // HMAC or Ed25519 (the WebSocket API then logs on)
  adapter::SymbolTable symbols;
  adapter::binance::VenueIdentity identity;
  model::Venue venue;               // of the ConnectionStatus records
  std::int64_t time_offset_ms = 0;  // the venue's clock minus the local one (startup checks)
  std::int64_t trades_since_ms = 0; // the node's start: earlier trades belong to no order it knows
  std::vector<adapter::binance::RecoveredOrder> recovered; // a resumed run's open orders
  std::string raw_frames; // user stream frames and WebSocket API answers; empty: none
  std::size_t ring_bytes = std::size_t{16} << 20U; // a snapshot record can take 1 MiB
  std::size_t command_slots = 4096;
  std::uint16_t source_id = 2;
  bool busy_poll = false;
  std::chrono::milliseconds snapshot_retry{2'000};
  std::chrono::milliseconds reconnect_initial{500};
  std::chrono::milliseconds reconnect_max{30'000};
  std::chrono::milliseconds rest_tick{1'000};    // the REST thread's listenKey check
  std::chrono::milliseconds check_every{60'000}; // the light check while synced; 0: off
  bool rest_fallback = true;                     // orders over REST while the WebSocket API is down
  adapter::binance::ListenKeyKeeperConfig keeper;
  std::function<std::int64_t()> now_ms; // local UTC milliseconds for signing; system clock if empty
  // persistence.mode = "barrier": the persist thread's durable log position; commands wait for it.
  const std::atomic<std::uint64_t>* durable = nullptr;
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
  std::uint64_t countdowns = 0;        // countdownCancelAll calls the venue took
  std::uint64_t countdown_failures = 0;
  std::uint64_t checks = 0; // light checks recorded
  std::uint64_t check_failures = 0;
  std::uint64_t rest_orders = 0;    // commands sent over REST (the WebSocket API was down)
  std::uint64_t barrier_waits = 0;  // commands that waited for their record to be durable
  std::uint64_t unsent_at_stop = 0; // commands still in the ring when the thread stopped (their
                                    // records never became durable: nothing was sent)
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

  [[nodiscard]] SpscByteRing& ring() noexcept;                // venue -> core
  [[nodiscard]] SpscRing<QueuedCommand>& commands() noexcept; // core -> venue
  [[nodiscard]] VenueIoStats stats() const noexcept;
  [[nodiscard]] std::uint16_t source_id() const noexcept;
  // The last error of the REST thread (listenKey, snapshot), for the operator.
  [[nodiscard]] std::string last_error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace jarvis::live
