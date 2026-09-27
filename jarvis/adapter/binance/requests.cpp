#include "jarvis/adapter/binance/requests.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>

#include <simdjson.h>

namespace jarvis::adapter::binance {

namespace {

namespace dom = simdjson::dom;
using core::Status;

template <typename T> std::string venue_text(const T& value) {
  std::array<char, 48> buf{};
  std::size_t n = 0;
  static_cast<void>(value.format(buf, n));
  return {buf.data(), n};
}

std::string_view side_text(model::OrderSide s) {
  return s == model::OrderSide::Buy ? "BUY" : "SELL";
}

// Keys the WebSocket API takes as numbers; everything else goes as a string.
bool numeric(std::string_view key) {
  return key == "timestamp" || key == "recvWindow" || key == "limit" || key == "orderId";
}

void json_string(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
}

std::uint64_t interval_ns(std::string_view unit, std::uint64_t n) {
  constexpr std::uint64_t kSecond = 1'000'000'000ULL;
  if (unit == "SECOND" || unit == "s" || unit == "S") {
    return n * kSecond;
  }
  if (unit == "MINUTE" || unit == "m" || unit == "M") {
    return n * 60 * kSecond;
  }
  if (unit == "HOUR" || unit == "h" || unit == "H") {
    return n * 3600 * kSecond;
  }
  if (unit == "DAY" || unit == "d" || unit == "D") {
    return n * 86400 * kSecond;
  }
  return 0;
}

bool ieq_prefix(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
                  std::tolower(static_cast<unsigned char>(b));
         });
}

std::string text_of(const dom::element& e, std::string_view key) {
  std::string_view v;
  return e[key].get_string().get(v) == simdjson::SUCCESS ? std::string{v} : std::string{};
}

std::optional<PlaceAck> ack_of(const dom::element& r) {
  std::uint64_t order_id = 0;
  if (r["orderId"].get_uint64().get(order_id) != simdjson::SUCCESS) {
    return std::nullopt;
  }
  PlaceAck a;
  a.order_id = order_id;
  a.client_order_id = text_of(r, "clientOrderId");
  a.status = text_of(r, "status");
  std::uint64_t t = 0;
  if (r["updateTime"].get_uint64().get(t) == simdjson::SUCCESS) {
    a.update_time_ms = t;
  }
  return a;
}

} // namespace

std::string url_encode(std::string_view text) {
  static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      out += ch;
    } else {
      out += '%';
      out += kHex[c >> 4U];
      out += kHex[c & 0x0FU];
    }
  }
  return out;
}

std::string query_string(const Params& params) {
  std::string out;
  for (const auto& [k, v] : params) {
    if (!out.empty()) {
      out += '&';
    }
    out += k;
    out += '=';
    out += url_encode(v);
  }
  return out;
}

Status place_params(const model::SubmitOrder& c, std::string_view symbol, Params& out,
                    std::string& error) {
  out.clear();
  out.emplace_back("symbol", std::string{symbol});
  out.emplace_back("side", std::string{side_text(c.order_side)});
  if (c.order_type == model::OrderType::Market) {
    out.emplace_back("type", "MARKET");
  } else if (c.order_type == model::OrderType::Limit) {
    if (!c.price) {
      error = "a limit order without a price";
      return Status::InvalidArgument;
    }
    out.emplace_back("type", "LIMIT");
    std::string_view tif;
    if (c.time_in_force == model::TimeInForce::Gtc) {
      tif = c.post_only ? "GTX" : "GTC";
    } else if (c.time_in_force == model::TimeInForce::Ioc) {
      tif = "IOC";
    } else if (c.time_in_force == model::TimeInForce::Fok) {
      tif = "FOK";
    } else {
      error = "a time in force the adapter does not send (v1: GTC, IOC, FOK, post-only GTX)";
      return Status::InvalidArgument;
    }
    out.emplace_back("timeInForce", std::string{tif});
    out.emplace_back("price", venue_text(*c.price));
  } else {
    error = "an order type the adapter does not send (v1: MARKET and LIMIT)";
    return Status::InvalidArgument;
  }
  out.emplace_back("quantity", venue_text(c.quantity));
  out.emplace_back("newClientOrderId", std::string{c.client_order_id.view()});
  if (c.reduce_only) {
    out.emplace_back("reduceOnly", "true");
  }
  out.emplace_back("newOrderRespType", "ACK");
  return Status::Ok;
}

Status modify_params(const model::ModifyOrder& c, std::string_view symbol, model::OrderSide side,
                     Params& out, std::string& /*error*/) {
  out.clear();
  out.emplace_back("symbol", std::string{symbol});
  out.emplace_back("origClientOrderId", std::string{c.client_order_id.view()});
  out.emplace_back("side", std::string{side_text(side)});
  out.emplace_back("quantity", venue_text(c.quantity));
  out.emplace_back("price", venue_text(c.price));
  return Status::Ok;
}

Status cancel_params(const model::CancelOrder& c, std::string_view symbol, Params& out) {
  out.clear();
  out.emplace_back("symbol", std::string{symbol});
  out.emplace_back("origClientOrderId", std::string{c.client_order_id.view()});
  return Status::Ok;
}

std::string signed_query(Params params, std::int64_t timestamp_ms, std::int64_t recv_window_ms,
                         const network::Signer& signer) {
  params.emplace_back("recvWindow", std::to_string(recv_window_ms));
  params.emplace_back("timestamp", std::to_string(timestamp_ms));
  std::string query = query_string(params);
  const std::string signature = signer.sign(query);
  query += "&signature=";
  query += signer.ed25519() ? url_encode(signature) : signature;
  return query;
}

std::string ws_request(std::string_view id, std::string_view method, Params params,
                       std::int64_t timestamp_ms, const network::Signer* signer,
                       std::string_view api_key) {
  if (signer != nullptr) {
    params.emplace_back("apiKey", std::string{api_key});
  }
  params.emplace_back("timestamp", std::to_string(timestamp_ms));
  if (signer != nullptr) {
    Params sorted = params;
    std::sort(sorted.begin(), sorted.end());
    std::string payload;
    for (const auto& [k, v] : sorted) {
      payload += payload.empty() ? "" : "&";
      payload += k;
      payload += '=';
      payload += v;
    }
    params.emplace_back("signature", signer->sign(payload));
  }
  std::string out = "{\"id\":";
  json_string(out, id);
  out += ",\"method\":";
  json_string(out, method);
  out += ",\"params\":{";
  bool first = true;
  for (const auto& [k, v] : params) {
    out += first ? "" : ",";
    first = false;
    json_string(out, k);
    out += ':';
    if (numeric(k)) {
      out += v;
    } else {
      json_string(out, v);
    }
  }
  out += "}}";
  return out;
}

Status decode_ws_answer(std::string_view json, core::UnixNanos recv, WsAnswer& out,
                        std::string& error) {
  out = WsAnswer{};
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "the WebSocket API answer is not JSON";
    return Status::ParseError;
  }
  std::uint64_t status = 0;
  if (doc["status"].get_uint64().get(status) != simdjson::SUCCESS) {
    error = "a WebSocket API answer without status";
    return Status::ParseError;
  }
  out.id = text_of(doc, "id");
  out.status = static_cast<int>(status);
  dom::element err;
  if (doc["error"].get(err) == simdjson::SUCCESS) {
    std::int64_t code = 0;
    if (err["code"].get_int64().get(code) == simdjson::SUCCESS) {
      out.error_code = static_cast<int>(code);
    }
    out.error_msg = text_of(err, "msg");
  }
  dom::element result;
  if (doc["result"].get(result) == simdjson::SUCCESS && result.is_object()) {
    out.ack = ack_of(result);
  }
  dom::array limits;
  if (doc["rateLimits"].get_array().get(limits) == simdjson::SUCCESS) {
    for (const dom::element l : limits) {
      const std::string type = text_of(l, "rateLimitType");
      std::uint64_t n = 0;
      std::uint64_t limit = 0;
      std::uint64_t count = 0;
      if (l["intervalNum"].get_uint64().get(n) != simdjson::SUCCESS ||
          l["count"].get_uint64().get(count) != simdjson::SUCCESS) {
        continue;
      }
      if (l["limit"].get_uint64().get(limit) != simdjson::SUCCESS) {
        limit = 0;
      }
      const std::uint64_t ns = interval_ns(text_of(l, "interval"), n);
      if (ns == 0 || (type != "ORDERS" && type != "REQUEST_WEIGHT")) {
        continue;
      }
      out.limits.push_back(model::RateLimitFeedback{
          type == "ORDERS" ? model::RateLimitKind::Orders : model::RateLimitKind::RequestWeight, ns,
          static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(limit), recv});
    }
  }
  return Status::Ok;
}

bool decode_rest_error(std::string_view json, int& code, std::string& msg) {
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  std::int64_t c = 0;
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS ||
      doc["code"].get_int64().get(c) != simdjson::SUCCESS) {
    return false;
  }
  code = static_cast<int>(c);
  msg = text_of(doc, "msg");
  return true;
}

Status decode_order_result(std::string_view json, PlaceAck& out, std::string& error) {
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "the order result is not JSON";
    return Status::ParseError;
  }
  const std::optional<PlaceAck> a = ack_of(doc);
  if (!a) {
    error = "an order result without orderId";
    return Status::ParseError;
  }
  out = *a;
  return Status::Ok;
}

void feedback_from_header(std::string_view name, std::string_view value, core::UnixNanos recv,
                          std::vector<model::RateLimitFeedback>& out) {
  model::RateLimitKind kind{};
  std::string_view suffix;
  if (ieq_prefix(name, "x-mbx-used-weight-")) {
    kind = model::RateLimitKind::RequestWeight;
    suffix = name.substr(18);
  } else if (ieq_prefix(name, "x-mbx-order-count-")) {
    kind = model::RateLimitKind::Orders;
    suffix = name.substr(18);
  } else {
    return;
  }
  std::uint64_t n = 0;
  const auto [end, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), n);
  std::uint64_t used = 0;
  const auto [vend, vec] = std::from_chars(value.data(), value.data() + value.size(), used);
  if (ec != std::errc{} || vec != std::errc{} || end == suffix.data() + suffix.size()) {
    return;
  }
  const std::uint64_t ns = interval_ns(std::string_view{end, 1}, n);
  if (ns != 0) {
    out.push_back(model::RateLimitFeedback{kind, ns, static_cast<std::uint32_t>(used), 0, recv});
  }
}

} // namespace jarvis::adapter::binance
