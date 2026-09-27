#include "jarvis/adapter/binance/rest_codec.hpp"

#include <vector>

#include <simdjson.h>

namespace jarvis::adapter::binance {

namespace {

namespace dom = simdjson::dom;
using core::Status;

Status levels_of(const dom::element& doc, std::string_view key, const SymbolEntry& symbol,
                 std::vector<BookLevel>& out, std::string& error) {
  out.clear();
  dom::array list;
  if (doc[key].get_array().get(list) != simdjson::SUCCESS) {
    error = "depth snapshot without " + std::string{key};
    return Status::ParseError;
  }
  for (const dom::element level : list) {
    dom::array pair;
    std::string_view px;
    std::string_view qty;
    if (level.get_array().get(pair) != simdjson::SUCCESS || pair.size() < 2 ||
        pair.at(0).get_string().get(px) != simdjson::SUCCESS ||
        pair.at(1).get_string().get(qty) != simdjson::SUCCESS) {
      error = "a malformed level in " + std::string{key};
      return Status::ParseError;
    }
    BookLevel l;
    if (!core::ok(exact_price(px, symbol.price_precision, l.price)) ||
        !core::ok(exact_quantity(qty, symbol.size_precision, l.size))) {
      error = "a level off the instrument's grid in " + std::string{key};
      return Status::PrecisionLoss;
    }
    out.push_back(l);
  }
  return Status::Ok;
}

Status snapshot_of(const dom::element& doc, const SymbolEntry& symbol, core::UnixNanos recv_ns,
                   DepthSnapshot& out, std::string& error) {
  std::uint64_t last = 0;
  std::uint64_t t = 0;
  if (doc["lastUpdateId"].get_uint64().get(last) != simdjson::SUCCESS) {
    error = "depth snapshot without lastUpdateId";
    return Status::ParseError;
  }
  if (doc["T"].get_uint64().get(t) != simdjson::SUCCESS) {
    t = 0; // Spot snapshots carry no transaction time
  }
  out.last_update_id = last;
  out.ts_event = core::UnixNanos{t * 1'000'000};
  out.ts_init = recv_ns;
  if (const Status s = levels_of(doc, "bids", symbol, out.bids, error); !core::ok(s)) {
    return s;
  }
  return levels_of(doc, "asks", symbol, out.asks, error);
}

} // namespace

Status decode_depth_snapshot(std::string_view json, const SymbolEntry& symbol,
                             core::UnixNanos recv_ns, DepthSnapshot& out, std::string& error) {
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "the depth snapshot is not JSON";
    return Status::ParseError;
  }
  return snapshot_of(doc, symbol, recv_ns, out, error);
}

Status decode_ws_depth_response(std::string_view json, const SymbolEntry& symbol,
                                core::UnixNanos recv_ns, std::string& id, DepthSnapshot& out,
                                std::string& error) {
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "the WebSocket API answer is not JSON";
    return Status::ParseError;
  }
  std::string_view id_text;
  std::uint64_t status = 0;
  if (doc["id"].get_string().get(id_text) != simdjson::SUCCESS ||
      doc["status"].get_uint64().get(status) != simdjson::SUCCESS) {
    error = "a WebSocket API answer without id or status";
    return Status::ParseError;
  }
  id.assign(id_text);
  if (status != 200) {
    std::string_view msg;
    if (doc["error"]["msg"].get_string().get(msg) != simdjson::SUCCESS) {
      msg = "no message";
    }
    error = "status " + std::to_string(status) + ": " + std::string{msg};
    return Status::InvalidArgument;
  }
  dom::element result;
  if (doc["result"].get(result) != simdjson::SUCCESS) {
    error = "a WebSocket API answer without result";
    return Status::ParseError;
  }
  return snapshot_of(result, symbol, recv_ns, out, error);
}

} // namespace jarvis::adapter::binance
