// A strategy plugin for the loader tests (tests/cpp/test_node.cpp, python/tests/test_node.py):
// one strategy that records its `scale` parameter when it starts and the price of every trade
// of its instruments (its [[strategies]] entry's, plus an `instrument` parameter).

#include <cstdint>
#include <string_view>
#include <vector>

#include "jarvis/core/status.hpp"
#include "jarvis/model/data.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/node/strategy_plugin.hpp"
#include "jarvis/node/strategy_registry.hpp"
#include "jarvis/strategy/context.hpp"

namespace {

struct PluginEcho {
  jarvis::model::Decimal scale;
  std::vector<jarvis::model::InstrumentId> instruments;

  static jarvis::core::Status create(const jarvis::node::StrategyParams& params, PluginEcho& out) {
    std::int64_t scale = 0;
    jarvis::core::Status s = params.get_or<std::int64_t>("scale", 1, scale);
    if (jarvis::core::ok(s)) {
      s = jarvis::model::Decimal::from_raw(scale * 1'000'000'000, 0, out.scale);
    }
    out.instruments.assign(params.instruments().begin(), params.instruments().end());
    std::string_view instrument;
    if (jarvis::core::ok(s) && jarvis::core::ok(params.get("instrument", instrument))) {
      jarvis::model::InstrumentId id;
      s = jarvis::model::InstrumentId::parse(instrument, id);
      out.instruments.push_back(id);
    }
    return s;
  }
  jarvis::core::Status on_start(jarvis::strategy::Context& ctx) {
    jarvis::core::Status s = ctx.record("scale", scale);
    for (const jarvis::model::InstrumentId& id : instruments) {
      if (jarvis::core::ok(s)) {
        s = ctx.subscribe_trades(id);
      }
    }
    return s;
  }
  static void on_trade(jarvis::strategy::Context& ctx, const jarvis::model::TradeTick& t) {
    jarvis::model::Decimal px;
    if (jarvis::core::ok(
            jarvis::model::Decimal::from_raw(t.price.raw(), t.price.precision(), px))) {
      static_cast<void>(ctx.record("px", px));
    }
  }
};

JARVIS_REGISTER_STRATEGY(PluginEcho, "plugin.Echo");

} // namespace

JARVIS_STRATEGY_PLUGIN();
