#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/execution/order_intent.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/node/config.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/sys/library.hpp"

// Native strategies in a shared library that a node loads at run time (docs/architecture.md
// section 7.3), so a Python node can host C++ strategies built outside the jarvis wheel:
//
//   // my_strategies.cpp, built with jarvis_add_strategy_plugin(my_strategies my_strategies.cpp)
//   struct PeggedMM { ... };
//   JARVIS_REGISTER_STRATEGY(PeggedMM, "PeggedMM");
//   JARVIS_STRATEGY_PLUGIN();
//
//   import jarvis
//   jarvis.load_native("build/libmy_strategies.so")   # ["PeggedMM"]
//   node.add_native_strategy("PeggedMM", params)
//
// The library and the node pass C++ objects to each other (the Context, the strategy function
// tables, StrategyParams, NativeStrategy), so both must be built from the same jarvis sources
// with the same compiler and flags. The entry point reports the compiler and a fingerprint of
// those types' layouts; the loader refuses a library whose report differs from its own. The
// library stays loaded for the life of the process: the strategies' code lives in it.

namespace jarvis::node {

inline constexpr std::uint32_t kPluginVersion = 1;
inline constexpr std::string_view kPluginEntry = "jarvis_strategy_plugin_v1";

struct PluginStrategy {
  const char* name = nullptr;
  core::Status (*create)(const StrategyParams& params, NativeStrategy& out) = nullptr;
};

// What the entry point returns.
struct PluginInfo {
  std::uint32_t version = 0;
  const char* compiler = nullptr; // __VERSION__ and the build's layout-changing flags
  std::uint64_t layout = 0;       // plugin_layout()
  const PluginStrategy* strategies = nullptr;
  std::size_t count = 0;
};

namespace detail {

[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
  constexpr std::uint64_t kPrime = 0x100000001b3ULL;
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xFFU;
    h *= kPrime;
  }
  return h;
}

template <typename... T> [[nodiscard]] constexpr std::uint64_t layout_of() noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  ((h = mix(mix(h, sizeof(T)), alignof(T))), ...);
  return h;
}

} // namespace detail

// A fingerprint of the layouts both sides share. It changes when any of these types changes
// size or alignment; a change that keeps them is caught only by the compiler check and by
// building the library from the same sources as the node.
[[nodiscard]] constexpr std::uint64_t plugin_layout() noexcept {
  return detail::mix(
      detail::layout_of<strategy::Context, strategy::KernelServices, strategy::StrategyVTable,
                        strategy::KernelConfig, StrategyParams, NativeStrategy, StrategyConfig,
                        Param, model::Event, model::Output, model::OrderEvent,
                        execution::OrderIntent, std::string, std::vector<int>>(),
      kPluginVersion);
}

namespace detail {

#if defined(__SANITIZE_ADDRESS__)
inline constexpr bool kAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
inline constexpr bool kAsan = true;
#else
inline constexpr bool kAsan = false;
#endif
#else
inline constexpr bool kAsan = false;
#endif
#if defined(_GLIBCXX_DEBUG)
inline constexpr bool kDebugContainers = true;
#else
inline constexpr bool kDebugContainers = false;
#endif

} // namespace detail

// The compiler and the flags that change layouts or runtimes (sanitizers, debug containers).
[[nodiscard]] inline const std::string& plugin_compiler() {
  static const std::string text = std::string{__VERSION__} + (detail::kAsan ? " asan" : "") +
                                  (detail::kDebugContainers ? " debug-containers" : "");
  return text;
}

// The library's side: its registered strategies, collected once.
[[nodiscard]] inline const PluginInfo* plugin_info() {
  static const std::vector<StrategyFactory> factories = [] {
    std::vector<StrategyFactory> out;
    const StrategyRegistry& registry = StrategyRegistry::instance();
    for (const std::string& name : registry.names()) {
      if (const StrategyFactory* f = registry.find(name)) {
        out.push_back(*f);
      }
    }
    return out;
  }();
  static const std::vector<PluginStrategy> strategies = [] {
    std::vector<PluginStrategy> out;
    out.reserve(factories.size());
    for (const StrategyFactory& f : factories) {
      out.push_back(PluginStrategy{f.name.c_str(), f.create});
    }
    return out;
  }();
  static const PluginInfo info{kPluginVersion, plugin_compiler().c_str(), plugin_layout(),
                               strategies.data(), strategies.size()};
  return &info;
}

// The node's side: loads the library at `path` and adds its strategies to `registry`; `names`
// lists them. Loading the same library again is harmless. Errors (nothing is added):
//   IoError            the library cannot be loaded;
//   InvalidArgument    it has no jarvis_strategy_plugin_v1 entry point;
//   InvalidState       another plugin version, compiler or layout: rebuild it against this node;
//   AlreadyExists      one of its names is registered by something else.
[[nodiscard]] core::Status load_strategy_plugin(const std::string& path, StrategyRegistry& registry,
                                                std::vector<std::string>& names,
                                                std::string& error);

// What the loader checks, on its own (for tests): Ok, or InvalidState with the reason.
[[nodiscard]] core::Status check_plugin_info(const PluginInfo& info, std::string& error);

} // namespace jarvis::node

// The entry point, once per plugin library.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define JARVIS_STRATEGY_PLUGIN()                                                                   \
  extern "C" JARVIS_EXPORT const ::jarvis::node::PluginInfo* jarvis_strategy_plugin_v1() {         \
    return ::jarvis::node::plugin_info();                                                          \
  }                                                                                                \
  static_assert(true, "")
