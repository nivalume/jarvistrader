#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy.hpp"

// Strategy sets (docs/architecture.md section 7.3). The engine talks to strategies only through
// this interface, indexed by StrategyIndex:
//
//   StaticStrategySet<S...>  every strategy type is known at compile time; calls are direct and
//                            inline (the pure C++ node);
//   DynamicStrategySet       strategies added at run time behind a StrategyVTable of function
//                            pointers (Python strategies and registered C++ strategies in the
//                            Python-launched node). This is the one indirect call the C++ subset
//                            allows (docs/cpp-subset.md).

namespace jarvis::strategy {

template <typename SS>
concept StrategySet =
    requires(SS& ss, StrategyIndex i, Context& ctx, const DataView& data, const BatchView& batch,
             core::TimerKey key, core::UnixNanos ts, const model::StrategyError& error,
             const model::OrderEvent& order_event) {
      { ss.size() } -> std::convertible_to<std::size_t>;
      { ss.on_start(i, ctx) } -> std::same_as<core::Status>;
      { ss.on_stop(i, ctx) } -> std::same_as<core::Status>;
      { ss.on_data(i, ctx, data) } -> std::same_as<core::Status>;
      { ss.on_batch(i, ctx, batch) } -> std::same_as<core::Status>;
      { ss.on_timer(i, ctx, key, ts) } -> std::same_as<core::Status>;
      { ss.on_error(i, ctx, error) } -> std::same_as<core::Status>;
      { ss.on_order_event(i, ctx, order_event) } -> std::same_as<core::Status>;
    };

template <Strategy... S> class StaticStrategySet {
public:
  explicit StaticStrategySet(S... strategies) : strategies_{std::move(strategies)...} {}

  [[nodiscard]] static constexpr std::size_t size() noexcept { return sizeof...(S); }

  template <std::size_t I> [[nodiscard]] auto& get() noexcept { return std::get<I>(strategies_); }

  core::Status on_start(StrategyIndex i, Context& ctx) {
    return visit(i, [&](auto& s) { return invoke_start(s, ctx); });
  }
  core::Status on_stop(StrategyIndex i, Context& ctx) {
    return visit(i, [&](auto& s) { return invoke_stop(s, ctx); });
  }
  core::Status on_data(StrategyIndex i, Context& ctx, const DataView& data) {
    return visit(i, [&](auto& s) { return invoke_data(s, ctx, data); });
  }
  core::Status on_batch(StrategyIndex i, Context& ctx, const BatchView& batch) {
    return visit(i, [&](auto& s) { return invoke_batch(s, ctx, batch); });
  }
  core::Status on_timer(StrategyIndex i, Context& ctx, core::TimerKey key, core::UnixNanos ts) {
    return visit(i, [&](auto& s) { return invoke_timer(s, ctx, key, ts); });
  }
  core::Status on_error(StrategyIndex i, Context& ctx, const model::StrategyError& error) {
    return visit(i, [&](auto& s) { return invoke_error(s, ctx, error); });
  }
  core::Status on_order_event(StrategyIndex i, Context& ctx, const model::OrderEvent& e) {
    return visit(i, [&](auto& s) { return invoke_order_event(s, ctx, e); });
  }

private:
  template <std::size_t I = 0, typename F> core::Status visit(StrategyIndex i, F&& f) {
    if constexpr (I == sizeof...(S)) {
      return core::Status::OutOfRange;
    } else {
      if (i == I) {
        return f(std::get<I>(strategies_));
      }
      return visit<I + 1>(i, std::forward<F>(f));
    }
  }

  std::tuple<S...> strategies_;
};

// Function table of one strategy type; nullptr-free (missing callbacks resolve to no-ops).
struct StrategyVTable {
  core::Status (*on_start)(void* self, Context& ctx);
  core::Status (*on_stop)(void* self, Context& ctx);
  core::Status (*on_data)(void* self, Context& ctx, const DataView& data);
  core::Status (*on_batch)(void* self, Context& ctx, const BatchView& batch);
  core::Status (*on_timer)(void* self, Context& ctx, core::TimerKey key, core::UnixNanos ts);
  core::Status (*on_error)(void* self, Context& ctx, const model::StrategyError& error);
  core::Status (*on_order_event)(void* self, Context& ctx, const model::OrderEvent& event);
};

template <Strategy S> [[nodiscard]] constexpr StrategyVTable make_vtable() noexcept {
  return StrategyVTable{
      [](void* self, Context& ctx) { return invoke_start(*static_cast<S*>(self), ctx); },
      [](void* self, Context& ctx) { return invoke_stop(*static_cast<S*>(self), ctx); },
      [](void* self, Context& ctx, const DataView& data) {
        return invoke_data(*static_cast<S*>(self), ctx, data);
      },
      [](void* self, Context& ctx, const BatchView& batch) {
        return invoke_batch(*static_cast<S*>(self), ctx, batch);
      },
      [](void* self, Context& ctx, core::TimerKey key, core::UnixNanos ts) {
        return invoke_timer(*static_cast<S*>(self), ctx, key, ts);
      },
      [](void* self, Context& ctx, const model::StrategyError& error) {
        return invoke_error(*static_cast<S*>(self), ctx, error);
      },
      [](void* self, Context& ctx, const model::OrderEvent& event) {
        return invoke_order_event(*static_cast<S*>(self), ctx, event);
      },
  };
}

// The vtable of S, one per type.
template <Strategy S> inline constexpr StrategyVTable kVTable = make_vtable<S>();

class DynamicStrategySet {
public:
  explicit DynamicStrategySet(std::size_t capacity) : entries_{capacity} {}

  // `self` is owned by the caller and must outlive the set.
  [[nodiscard]] core::Status add(void* self, const StrategyVTable* vtable) noexcept {
    return entries_.push_back(Entry{self, vtable});
  }
  template <Strategy S> [[nodiscard]] core::Status add(S& strategy) noexcept {
    return add(&strategy, &kVTable<S>);
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  core::Status on_start(StrategyIndex i, Context& ctx) {
    return entries_[i].vtable->on_start(entries_[i].self, ctx);
  }
  core::Status on_stop(StrategyIndex i, Context& ctx) {
    return entries_[i].vtable->on_stop(entries_[i].self, ctx);
  }
  core::Status on_data(StrategyIndex i, Context& ctx, const DataView& data) {
    return entries_[i].vtable->on_data(entries_[i].self, ctx, data);
  }
  core::Status on_batch(StrategyIndex i, Context& ctx, const BatchView& batch) {
    return entries_[i].vtable->on_batch(entries_[i].self, ctx, batch);
  }
  core::Status on_timer(StrategyIndex i, Context& ctx, core::TimerKey key, core::UnixNanos ts) {
    return entries_[i].vtable->on_timer(entries_[i].self, ctx, key, ts);
  }
  core::Status on_error(StrategyIndex i, Context& ctx, const model::StrategyError& error) {
    return entries_[i].vtable->on_error(entries_[i].self, ctx, error);
  }
  core::Status on_order_event(StrategyIndex i, Context& ctx, const model::OrderEvent& e) {
    return entries_[i].vtable->on_order_event(entries_[i].self, ctx, e);
  }

private:
  struct Entry {
    void* self;
    const StrategyVTable* vtable;
  };
  core::FixedVector<Entry> entries_;
};

static_assert(StrategySet<DynamicStrategySet>);

} // namespace jarvis::strategy
