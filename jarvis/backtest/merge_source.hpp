#pragma once

#include <cstddef>

#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/model/event.hpp"

// Merges several event sources into one stream in key order (docs/architecture.md section 5.2).
// Each source must yield strictly increasing keys; the merge checks this and reports
// InvalidArgument when a source goes backwards. Equal keys from different sources are served
// in the order the sources were added, so the merge is deterministic even for sloppy inputs.
//
// A source's event may borrow its storage (OrderBookDeltas, AccountState point into the
// source's decode scratch). The merge therefore advances a source only on the call after the
// one that returned its event: the returned event stays valid until the next `next()`.

namespace jarvis::backtest {

template <engine::EventSource S> class MergeSource {
public:
  explicit MergeSource(std::size_t capacity) : heads_{capacity} {}

  // Adds a source before the first next(); `source` must outlive the merge.
  [[nodiscard]] core::Status add(S& source) noexcept {
    if (primed_) {
      return core::Status::InvalidState;
    }
    return heads_.push_back(Head{&source, {}, {}, true, false});
  }

  [[nodiscard]] std::size_t size() const noexcept { return heads_.size(); }

  [[nodiscard]] core::Status next(core::EventKey& key, model::Event& event) {
    if (!primed_) {
      primed_ = true;
      for (std::size_t i = 0; i < heads_.size(); ++i) {
        const core::Status s = pull(heads_[i]);
        if (!core::ok(s)) {
          return s;
        }
      }
    } else if (last_ < heads_.size()) {
      const core::Status s = pull(heads_[last_]);
      if (!core::ok(s)) {
        return s;
      }
    }
    std::size_t best = heads_.size();
    for (std::size_t i = 0; i < heads_.size(); ++i) {
      if (heads_[i].live && (best == heads_.size() || heads_[i].key < heads_[best].key)) {
        best = i;
      }
    }
    last_ = best;
    if (best == heads_.size()) {
      return core::Status::EndOfStream;
    }
    key = heads_[best].key;
    event = heads_[best].event;
    return core::Status::Ok;
  }

private:
  struct Head {
    S* source;
    core::EventKey key;
    model::Event event;
    bool live;
    bool started;
  };

  static core::Status pull(Head& head) {
    if (!head.live) {
      return core::Status::Ok;
    }
    core::EventKey key;
    const core::Status s = head.source->next(key, head.event);
    if (s == core::Status::EndOfStream) {
      head.live = false;
      return core::Status::Ok;
    }
    if (!core::ok(s)) {
      return s;
    }
    if (head.started && !(head.key < key)) {
      return core::Status::InvalidArgument; // the source is not in key order
    }
    head.key = key;
    head.started = true;
    return core::Status::Ok;
  }

  core::FixedVector<Head> heads_;
  std::size_t last_ = static_cast<std::size_t>(-1);
  bool primed_ = false;
};

} // namespace jarvis::backtest
