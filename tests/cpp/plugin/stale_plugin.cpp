// A strategy plugin built "from other sources": its entry point reports another layout, so the
// loader must refuse it (tests/cpp/test_node.cpp).

#include "jarvis/node/strategy_plugin.hpp"

extern "C" __attribute__((visibility("default"))) const jarvis::node::PluginInfo*
jarvis_strategy_plugin_v1() {
  static const jarvis::node::PluginInfo info{jarvis::node::kPluginVersion,
                                             jarvis::node::plugin_compiler().c_str(),
                                             jarvis::node::plugin_layout() + 1, nullptr, 0};
  return &info;
}
