#include "jarvis/node/strategy_plugin.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/sys/library.hpp"

namespace jarvis::node {

using core::Status;

Status check_plugin_info(const PluginInfo& info, std::string& error) {
  if (info.version != kPluginVersion) {
    error = "plugin version " + std::to_string(info.version) + ", the node expects " +
            std::to_string(kPluginVersion);
    return Status::InvalidState;
  }
  const std::string_view compiler = info.compiler != nullptr ? info.compiler : "";
  if (compiler != plugin_compiler()) {
    error = "built with '" + std::string{compiler} + "', the node with '" + plugin_compiler() +
            "'; rebuild the plugin against this node";
    return Status::InvalidState;
  }
  if (info.layout != plugin_layout()) {
    error = "built from other jarvis sources (the shared types differ); rebuild the plugin "
            "against this node";
    return Status::InvalidState;
  }
  if (info.count != 0 && info.strategies == nullptr) {
    error = "the plugin lists strategies without a table";
    return Status::InvalidState;
  }
  return Status::Ok;
}

namespace {

// The plugin's strategies against the registry: a name registered with another factory is a
// conflict; one registered with the same factory was loaded before.
Status check_names(const PluginInfo& info, const StrategyRegistry& registry,
                   std::vector<std::string>& names, std::string& error) {
  names.clear();
  for (std::size_t i = 0; i < info.count; ++i) {
    const PluginStrategy& p = info.strategies[i];
    if (p.name == nullptr || *p.name == '\0' || p.create == nullptr) {
      error = "the plugin lists a strategy without a name or factory";
      return Status::InvalidState;
    }
    const StrategyFactory* existing = registry.find(p.name);
    if (existing != nullptr && existing->create != p.create) {
      error = "strategy '" + std::string{p.name} + "' is already registered";
      return Status::AlreadyExists;
    }
    names.emplace_back(p.name);
  }
  return Status::Ok;
}

} // namespace

Status load_strategy_plugin(const std::string& path, StrategyRegistry& registry,
                            std::vector<std::string>& names, std::string& error) {
  names.clear();
  // The plugin's copies of the jarvis code stay its own. A library that loads is never unloaded
  // (the loader counts references, so loading the same library again is cheap).
  std::string why_not;
  void* handle = sys::load_library(path, why_not);
  if (handle == nullptr) {
    error = "cannot load " + path + ": " + why_not;
    return Status::IoError;
  }
  const std::string entry{kPluginEntry};
  void* symbol = sys::library_symbol(handle, entry.c_str());
  if (symbol == nullptr) {
    sys::unload_library(handle);
    error = path + " is not a jarvis strategy plugin (no " + entry +
            "; add JARVIS_STRATEGY_PLUGIN() to one of its sources)";
    return Status::InvalidArgument;
  }
  using Entry = const PluginInfo* (*)();
  // The one cast the loader needs: a function's address comes back as void*.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const PluginInfo* info = reinterpret_cast<Entry>(symbol)();
  std::string why;
  Status s = info != nullptr ? check_plugin_info(*info, why) : Status::InvalidState;
  if (info == nullptr) {
    why = "the entry point returned nothing";
  }
  if (core::ok(s)) {
    s = check_names(*info, registry, names, why);
  }
  if (!core::ok(s)) {
    sys::unload_library(handle);
    names.clear();
    error = path + ": " + why;
    return s;
  }
  for (std::size_t i = 0; i < info->count; ++i) {
    const PluginStrategy& p = info->strategies[i];
    if (registry.find(p.name) == nullptr) {
      static_cast<void>(registry.add(StrategyFactory{p.name, p.create}));
    }
  }
  return Status::Ok;
}

} // namespace jarvis::node
