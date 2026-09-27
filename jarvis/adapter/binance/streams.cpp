#include "jarvis/adapter/binance/streams.hpp"

namespace jarvis::adapter::binance {

Route route_of(std::string_view stream) {
  const std::size_t at = stream.find('@');
  const std::string_view kind = at == std::string_view::npos ? stream : stream.substr(at + 1);
  // The high-frequency book streams live on /public; everything else on /market.
  if (kind.starts_with("bookTicker") || kind.starts_with("depth")) {
    return Route::Public;
  }
  return Route::Market;
}

std::vector<StreamConnection> stream_connections(std::string_view base,
                                                 std::span<const std::string> streams) {
  std::vector<StreamConnection> out;
  for (const Route route : {Route::Public, Route::Market}) {
    StreamConnection* current = nullptr;
    for (const std::string& s : streams) {
      if (route_of(s) != route) {
        continue;
      }
      if (current == nullptr || current->streams.size() == kMaxStreamsPerConnection) {
        out.push_back(StreamConnection{route, {}, {}});
        current = &out.back();
      }
      current->streams.push_back(s);
    }
  }
  for (StreamConnection& c : out) {
    c.url.assign(base);
    c.url += c.route == Route::Public ? "/public/stream?streams=" : "/market/stream?streams=";
    for (std::size_t i = 0; i < c.streams.size(); ++i) {
      if (i > 0) {
        c.url += '/';
      }
      c.url += c.streams[i];
    }
  }
  return out;
}

} // namespace jarvis::adapter::binance
