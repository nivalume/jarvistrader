#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/wire.hpp"

// The core thread's end of the market data hand-off (docs/architecture.md sections 4.1 and
// 7.1): records the IO threads wrote to their rings are decoded here, stamped with the core
// clock, and served to the driver in arrival order. Nothing yet is WouldBlock, never the end.
// An event's book deltas live in the source until the driver asks for the event after it.

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
    } else if (std::holds_alternative<model::AccountState>(e.event)) {
      return core::Status::UnsupportedMessage; // not market data
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
    }
    key = current_.key;
    event = current_.event;
    return core::Status::Ok;
  }

  [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
  [[nodiscard]] std::uint64_t pushed() const noexcept { return pushed_; }

private:
  struct Entry {
    core::EventKey key;
    model::Event event;
    std::vector<model::OrderBookDelta> deltas;
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
