#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Market stream routing of the USDⓈ-M WebSocket endpoints (docs/architecture.md section 14.1).
// Checked against fstream.binance.com on 2026-09-27: /public/stream delivers bookTicker and
// depth streams only, /market/stream delivers aggTrade, markPrice and kline (and forceOrder), and
// the legacy /stream path now delivers only the /public streams. A stream subscribed on the
// wrong route is silently empty, so the adapter always splits subscriptions by route.

namespace jarvis::adapter::binance {

enum class Route : unsigned char { Public, Market };

inline constexpr std::size_t kMaxStreamsPerConnection = 1024;

// The route of a stream name such as "btcusdt@depth@100ms" or "!forceOrder@arr".
[[nodiscard]] Route route_of(std::string_view stream);

struct StreamConnection {
  Route route = Route::Public;
  std::string url; // base + "/public/stream?streams=a/b/..."
  std::vector<std::string> streams;
};

// Groups streams by route, at most kMaxStreamsPerConnection per connection, in the order given.
// `base` is the endpoint without a path, e.g. "wss://fstream.binance.com".
[[nodiscard]] std::vector<StreamConnection>
stream_connections(std::string_view base, std::span<const std::string> streams);

} // namespace jarvis::adapter::binance
