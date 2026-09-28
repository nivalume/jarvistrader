#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"

// The core thread's end of the IO threads' hand-off (docs/architecture.md sections 4.1 and 7.1):
// records the IO threads wrote to their rings are decoded here, stamped with the core clock,
// and served to the driver in arrival order. Nothing yet is WouldBlock, never the end. An
// event's lists (book deltas; an account state's balances; a venue snapshot's reports) live in
// the source until the driver asks for the event after it.

namespace jarvis::live {

class LiveSource {
public:
  explicit LiveSource(std::size_t max_deltas = 16384) : scratch_{max_deltas} {}

  // Decodes one wire record (as encode_record wrote it) and queues it at `ts`.
  [[nodiscard]] core::Status push_record(std::span<const std::byte> record, core::UnixNanos ts,
                                         std::uint16_t source_id) {
    model::wire::RecordView view;
    core::Status s = model::wire::decode_record(record, view);
    if (!core::ok(s)) {
      return s;
    }
    scratch_.deltas.clear();
    scratch_.balances.clear();
    scratch_.margins.clear();
    Entry e;
    s = model::wire::decode_event(view, scratch_, e.event);
    if (!core::ok(s)) {
      return s;
    }
    if (auto* d = std::get_if<model::OrderBookDeltas>(&e.event)) {
      e.deltas = spare();
      e.deltas.assign(d->deltas.begin(), d->deltas.end());
    } else if (const auto* a = std::get_if<model::AccountState>(&e.event)) {
      e.lists = std::make_unique<Lists>();
      e.lists->balances.assign(a->balances.begin(), a->balances.end());
      e.lists->margins.assign(a->margins.begin(), a->margins.end());
    } else if (const auto* v = std::get_if<model::VenueSnapshot>(&e.event)) {
      e.lists = std::make_unique<Lists>();
      e.lists->balances.assign(v->balances.begin(), v->balances.end());
      e.lists->orders.assign(v->orders.begin(), v->orders.end());
      e.lists->fills.assign(v->fills.begin(), v->fills.end());
      e.lists->positions.assign(v->positions.begin(), v->positions.end());
    }
    e.key = core::EventKey{ts, source_id, ++pushed_};
    queue_.push_back(std::move(e));
    return core::Status::Ok;
  }

  [[nodiscard]] core::Status next(core::EventKey& key, model::Event& event) {
    if (queue_.empty()) {
      return core::Status::WouldBlock;
    }
    if (!current_.deltas.empty() || current_.deltas.capacity() > 0) {
      spare_.push_back(std::move(current_.deltas)); // the previous event has been stepped
    }
    current_ = std::move(queue_.front());
    queue_.pop_front();
    if (auto* d = std::get_if<model::OrderBookDeltas>(&current_.event)) {
      d->deltas = std::span<const model::OrderBookDelta>{current_.deltas};
    } else if (auto* a = std::get_if<model::AccountState>(&current_.event)) {
      a->balances = current_.lists->balances;
      a->margins = current_.lists->margins;
    } else if (auto* v = std::get_if<model::VenueSnapshot>(&current_.event)) {
      v->balances = current_.lists->balances;
      v->orders = current_.lists->orders;
      v->fills = current_.lists->fills;
      v->positions = current_.lists->positions;
    }
    key = current_.key;
    event = current_.event;
    return core::Status::Ok;
  }

  [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
  [[nodiscard]] std::uint64_t pushed() const noexcept { return pushed_; }

private:
  // The lists of the rare account records (allocated per record; market data never is).
  struct Lists {
    std::vector<model::AccountBalance> balances;
    std::vector<model::MarginBalance> margins;
    std::vector<model::OrderStatusReport> orders;
    std::vector<model::FillReport> fills;
    std::vector<model::PositionStatusReport> positions;
  };

  struct Entry {
    core::EventKey key;
    model::Event event;
    std::vector<model::OrderBookDelta> deltas;
    std::unique_ptr<Lists> lists;
  };

  std::vector<model::OrderBookDelta> spare() {
    if (spare_.empty()) {
      return {};
    }
    std::vector<model::OrderBookDelta> v = std::move(spare_.back());
    spare_.pop_back();
    v.clear();
    return v;
  }

  model::wire::DecodeScratch scratch_;
  std::deque<Entry> queue_;
  Entry current_;
  std::vector<std::vector<model::OrderBookDelta>> spare_; // buffers to reuse
  std::uint64_t pushed_ = 0;
};

} // namespace jarvis::live
