#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/strategy/strategy.hpp"
#include "jarvis/strategy/strategy_set.hpp"

// Native (C++) strategies registered by name (docs/architecture.md section 7.3), so that
// `impl = "cpp:<name>"` in [[strategies]] and Node.add_native_strategy(name, ...) in Python can
// create them. A strategy registers itself in its own translation unit:
//
//   struct PeggedMM { ... };
//   JARVIS_REGISTER_STRATEGY(PeggedMM, "PeggedMM");
//
// Registration runs during static initialisation, so the translation unit must be linked into
// the executable or shared library directly (an object file in a static library that nothing
// references is dropped by the linker).

namespace jarvis::node {

// Read-only view of one [[strategies]] entry: its id, instruments and typed parameters.
class StrategyParams {
public:
  explicit StrategyParams(const StrategyConfig& config) noexcept : config_{&config} {}

  [[nodiscard]] std::string_view id() const noexcept { return config_->id; }
  [[nodiscard]] std::span<const model::InstrumentId> instruments() const noexcept {
    return config_->instruments;
  }
  [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }

  // Each getter returns NotFound when the key is absent and InvalidArgument when the value has
  // another type (a Decimal also accepts an integer, and parses a string such as "0.010").
  [[nodiscard]] core::Status get(std::string_view key, bool& out) const noexcept;
  [[nodiscard]] core::Status get(std::string_view key, std::int64_t& out) const noexcept;
  [[nodiscard]] core::Status get(std::string_view key, std::string_view& out) const noexcept;
  [[nodiscard]] core::Status get(std::string_view key, model::Decimal& out) const noexcept;

  // The value, or `fallback` when the key is absent. A value of the wrong type is an error.
  template <typename T>
  [[nodiscard]] core::Status get_or(std::string_view key, T fallback, T& out) const noexcept {
    const core::Status s = get(key, out);
    if (s == core::Status::NotFound) {
      out = fallback;
      return core::Status::Ok;
    }
    return s;
  }

private:
  [[nodiscard]] const Param* find(std::string_view key) const noexcept;

  const StrategyConfig* config_;
};

// An owned instance of a registered strategy, addressed through its function table.
class NativeStrategy {
public:
  NativeStrategy() = default;
  NativeStrategy(void* self, void (*destroy)(void*), const strategy::StrategyVTable* vtable,
                 std::string name)
      : self_{self, destroy}, vtable_{vtable}, name_{std::move(name)} {}

  [[nodiscard]] void* self() const noexcept { return self_.get(); }
  [[nodiscard]] const strategy::StrategyVTable* vtable() const noexcept { return vtable_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }
  [[nodiscard]] explicit operator bool() const noexcept { return self_ != nullptr; }

  // Adds this instance to `set`; the instance must outlive the set.
  [[nodiscard]] core::Status add_to(strategy::DynamicStrategySet& set) const noexcept {
    return set.add(self_.get(), vtable_);
  }

private:
  std::unique_ptr<void, void (*)(void*)> self_{nullptr, nullptr};
  const strategy::StrategyVTable* vtable_ = nullptr;
  std::string name_;
};

struct StrategyFactory {
  std::string name;
  core::Status (*create)(const StrategyParams& params, NativeStrategy& out) = nullptr;
};

// The process-wide table of registered strategies.
class StrategyRegistry {
public:
  [[nodiscard]] static StrategyRegistry& instance();

  // False when `name` is empty or already registered; the duplicate is remembered and
  // reported by check() so a node refuses to start instead of picking one silently.
  bool add(StrategyFactory factory);

  [[nodiscard]] const StrategyFactory* find(std::string_view name) const noexcept;
  [[nodiscard]] std::vector<std::string> names() const;
  // Ok, or AlreadyExists naming the first duplicate registration in `detail`.
  [[nodiscard]] core::Status check(std::string& detail) const;

  // Creates the strategy registered as `name` from `params`. NotFound for an unknown name.
  [[nodiscard]] core::Status create(std::string_view name, const StrategyParams& params,
                                    NativeStrategy& out) const;

private:
  std::vector<StrategyFactory> factories_;
  std::vector<std::string> duplicates_;
};

namespace detail {

template <typename S>
concept CreatableFromParams = requires(const StrategyParams& p, S& out) {
  { S::create(p, out) } -> std::same_as<core::Status>;
};

// Construction, most specific first: `static Status S::create(const StrategyParams&, S&)`, a
// constructor taking `const StrategyParams&`, or the default constructor.
template <strategy::Strategy S>
core::Status create_native(const StrategyParams& params, NativeStrategy& out) {
  std::unique_ptr<S> instance;
  if constexpr (CreatableFromParams<S>) {
    static_assert(std::is_default_constructible_v<S>,
                  "S::create(params, out) needs S to be default constructible");
    instance = std::make_unique<S>();
    const core::Status s = S::create(params, *instance);
    if (!core::ok(s)) {
      return s;
    }
  } else if constexpr (std::is_constructible_v<S, const StrategyParams&>) {
    instance = std::make_unique<S>(params);
  } else {
    static_assert(std::is_default_constructible_v<S>,
                  "a registered strategy needs S::create, S(const StrategyParams&) or S()");
    instance = std::make_unique<S>();
  }
  out = NativeStrategy{instance.release(), [](void* p) { delete static_cast<S*>(p); },
                       &strategy::kVTable<S>, std::string{}};
  return core::Status::Ok;
}

} // namespace detail

template <strategy::Strategy S> bool register_strategy(std::string_view name) {
  return StrategyRegistry::instance().add(
      StrategyFactory{std::string{name}, &detail::create_native<S>});
}

} // namespace jarvis::node

#define JARVIS_STRATEGY_CONCAT_INNER(a, b) a##b
#define JARVIS_STRATEGY_CONCAT(a, b) JARVIS_STRATEGY_CONCAT_INNER(a, b)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define JARVIS_REGISTER_STRATEGY(Type, Name)                                                       \
  [[maybe_unused]] static const bool JARVIS_STRATEGY_CONCAT(                                       \
      jarvis_strategy_registered_, __COUNTER__) = ::jarvis::node::register_strategy<Type>(Name)
