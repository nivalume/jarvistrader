#include "jarvis/adapter/binance/rest_client.hpp"

#include <array>
#include <charconv>

#include <simdjson.h>

namespace jarvis::adapter::binance {

namespace {

namespace dom = simdjson::dom;
using core::Status;

constexpr std::int64_t kDefaultBanMs = 120'000; // 418 without Retry-After: two minutes

std::int64_t system_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Status parse_json(dom::parser& parser, std::string_view body, dom::element& doc,
                  std::string& error) {
  const simdjson::padded_string padded{body};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "the response is not JSON";
    return Status::ParseError;
  }
  return Status::Ok;
}

// The body of a call that succeeded, parsed; a failed call's status passes through.
Status parsed(Status called, dom::parser& parser, const RestResponse& r, dom::element& doc,
              std::string& error) {
  if (!core::ok(called)) {
    return called;
  }
  return parse_json(parser, r.body, doc, error);
}

std::string param(const Params& params, std::string_view key) {
  for (const auto& [k, v] : params) {
    if (k == key) {
      return v;
    }
  }
  return {};
}

} // namespace

RestClient::RestClient(RestConfig config)
    : config_{std::move(config)},
      http_{network::HttpsConfig{config_.base_url, config_.tls, config_.timeout}} {}

std::int64_t RestClient::now_ms() const { return config_.now_ms ? config_.now_ms() : system_ms(); }

bool RestClient::banned() const { return now_ms() < banned_until_ms_; }

std::vector<model::RateLimitFeedback> RestClient::take_limits() {
  std::vector<model::RateLimitFeedback> out;
  out.swap(limits_);
  return out;
}

Status RestClient::call(std::string_view method, std::string_view path, Params params,
                        Security security, RestResponse& out, std::string& error) {
  out = RestResponse{};
  if (banned()) {
    error = "the IP is banned by the venue (HTTP 418) for another " +
            std::to_string((banned_until_ms_ - now_ms()) / 1000) + " s";
    return Status::InvalidState;
  }
  std::string target{path};
  std::string query;
  if (security == Security::Signed) {
    query = signed_query(std::move(params), now_ms() + offset_ms_, config_.recv_window_ms,
                         config_.signer);
  } else {
    query = query_string(params);
  }
  if (!query.empty()) {
    target += '?';
    target += query;
  }
  std::vector<network::HeaderField> headers;
  if (security != Security::None) {
    headers.push_back({"X-MBX-APIKEY", config_.api_key});
  }
  network::HttpResponse r;
  const Status s = http_.request(method, target, headers, {}, r, error);
  if (!core::ok(s)) {
    return s;
  }
  const core::UnixNanos recv{static_cast<std::uint64_t>(now_ms()) * 1'000'000};
  out.status = r.status;
  out.body = std::move(r.body);
  for (const network::HeaderField& h : r.headers) {
    feedback_from_header(h.name, h.value, recv, out.limits);
  }
  if (const std::optional<std::string_view> ra = r.header("Retry-After")) {
    std::int64_t seconds = 0;
    if (std::from_chars(ra->data(), ra->data() + ra->size(), seconds).ec == std::errc{}) {
      out.retry_after_s = seconds;
    }
  }
  if (out.status == 418) {
    banned_until_ms_ = now_ms() + (out.retry_after_s ? *out.retry_after_s * 1000 : kDefaultBanMs);
    http_418_.fetch_add(1, std::memory_order_relaxed);
  } else if (out.status == 429) {
    http_429_.fetch_add(1, std::memory_order_relaxed);
  }
  if (out.status >= 400) {
    static_cast<void>(decode_rest_error(out.body, out.error_code, out.error_msg));
  }
  limits_.insert(limits_.end(), out.limits.begin(), out.limits.end());
  return Status::Ok;
}

Status RestClient::json_call(std::string_view method, std::string_view path, Params params,
                             Security security, RestResponse& out, std::string& error) {
  const Status s = call(method, path, std::move(params), security, out, error);
  if (!core::ok(s)) {
    return s;
  }
  if (out.status != 200) {
    error = std::string{path} + ": HTTP " + std::to_string(out.status) +
            (out.error_code != 0 ? " code " + std::to_string(out.error_code) + " " + out.error_msg
                                 : std::string{});
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

Status RestClient::sync_time(std::string& error) {
  RestResponse r;
  const Status called = json_call("GET", "/fapi/v1/time", {}, Security::None, r, error);
  const std::int64_t after = now_ms();
  dom::parser parser;
  dom::element doc;
  if (const Status s = parsed(called, parser, r, doc, error); !core::ok(s)) {
    return s;
  }
  std::int64_t server = 0;
  if (doc["serverTime"].get_int64().get(server) != simdjson::SUCCESS) {
    error = "serverTime missing";
    return Status::ParseError;
  }
  // The venue read its clock during the call, before `after` (local time), so server - after is
  // a lower bound of the offset. The lower bound: a local estimate of the venue's clock
  // (a snapshot's T_s above all) must never run ahead of it, or events the venue stamps just
  // after T_s would count as already in the snapshot and be dropped as stale; one that runs
  // behind only applies such events again, which is idempotent. Signed requests stay within
  // recvWindow either way.
  offset_ms_ = server - after;
  return Status::Ok;
}

Status RestClient::position_mode(bool& dual, std::string& error) {
  RestResponse r;
  dom::parser parser;
  dom::element doc;
  if (const Status s =
          parsed(json_call("GET", "/fapi/v1/positionSide/dual", {}, Security::Signed, r, error),
                 parser, r, doc, error);
      !core::ok(s)) {
    return s;
  }
  if (doc["dualSidePosition"].get_bool().get(dual) != simdjson::SUCCESS) {
    error = "dualSidePosition missing";
    return Status::ParseError;
  }
  return Status::Ok;
}

Status RestClient::multi_assets_mode(bool& on, std::string& error) {
  RestResponse r;
  dom::parser parser;
  dom::element doc;
  if (const Status s =
          parsed(json_call("GET", "/fapi/v1/multiAssetsMargin", {}, Security::Signed, r, error),
                 parser, r, doc, error);
      !core::ok(s)) {
    return s;
  }
  if (doc["multiAssetsMargin"].get_bool().get(on) != simdjson::SUCCESS) {
    error = "multiAssetsMargin missing";
    return Status::ParseError;
  }
  return Status::Ok;
}

Status RestClient::position_risk(std::string_view symbol, PositionRisk& out, std::string& error) {
  RestResponse r;
  dom::parser parser;
  dom::element doc;
  if (const Status s =
          parsed(json_call("GET", "/fapi/v2/positionRisk", {{"symbol", std::string{symbol}}},
                           Security::Signed, r, error),
                 parser, r, doc, error);
      !core::ok(s)) {
    return s;
  }
  dom::array list;
  if (doc.get_array().get(list) != simdjson::SUCCESS) {
    error = "positionRisk is not a list";
    return Status::ParseError;
  }
  for (const dom::element p : list) {
    std::string_view sym;
    std::string_view leverage;
    std::string_view margin;
    std::string_view amount;
    if (p["symbol"].get_string().get(sym) != simdjson::SUCCESS || sym != symbol) {
      continue;
    }
    if (p["leverage"].get_string().get(leverage) != simdjson::SUCCESS ||
        p["marginType"].get_string().get(margin) != simdjson::SUCCESS) {
      error = "positionRisk entry without leverage or marginType";
      return Status::ParseError;
    }
    if (p["positionAmt"].get_string().get(amount) != simdjson::SUCCESS) {
      amount = "0";
    }
    out.symbol.assign(sym);
    out.leverage = 0;
    static_cast<void>(
        std::from_chars(leverage.data(), leverage.data() + leverage.size(), out.leverage));
    out.margin_type.assign(margin);
    out.position_amount.assign(amount);
    return Status::Ok; // one-way mode: one entry per symbol
  }
  error = std::string{symbol} + " is not in positionRisk";
  return Status::NotFound;
}

Status RestClient::create_listen_key(std::string& key, std::string& error) {
  RestResponse r;
  dom::parser parser;
  dom::element doc;
  if (const Status s =
          parsed(json_call("POST", "/fapi/v1/listenKey", {}, Security::ApiKey, r, error), parser, r,
                 doc, error);
      !core::ok(s)) {
    return s;
  }
  std::string_view k;
  if (doc["listenKey"].get_string().get(k) != simdjson::SUCCESS) {
    error = "listenKey missing";
    return Status::ParseError;
  }
  key.assign(k);
  return Status::Ok;
}

Status RestClient::keep_alive_listen_key(std::string& error) {
  RestResponse r;
  return json_call("PUT", "/fapi/v1/listenKey", {}, Security::ApiKey, r, error);
}

Status RestClient::close_listen_key(std::string& error) {
  RestResponse r;
  return json_call("DELETE", "/fapi/v1/listenKey", {}, Security::ApiKey, r, error);
}

Status RestClient::countdown_cancel_all(std::string_view symbol, std::uint32_t countdown_ms,
                                        std::string& error) {
  RestResponse r;
  return json_call(
      "POST", "/fapi/v1/countdownCancelAll",
      {{"symbol", std::string{symbol}}, {"countdownTime", std::to_string(countdown_ms)}},
      Security::Signed, r, error);
}

Status RestClient::exchange_info(std::string& body, std::string& error) {
  RestResponse r;
  const Status s = json_call("GET", "/fapi/v1/exchangeInfo", {}, Security::None, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::key_restrictions(KeyRestrictions& out, std::string& error) {
  RestResponse r;
  dom::parser parser;
  dom::element doc;
  if (const Status s = parsed(
          json_call("GET", "/sapi/v1/account/apiRestrictions", {}, Security::Signed, r, error),
          parser, r, doc, error);
      !core::ok(s)) {
    return s;
  }
  if (doc["ipRestrict"].get_bool().get(out.ip_restrict) != simdjson::SUCCESS ||
      doc["enableFutures"].get_bool().get(out.enable_futures) != simdjson::SUCCESS ||
      doc["enableWithdrawals"].get_bool().get(out.enable_withdrawals) != simdjson::SUCCESS ||
      doc["enableReading"].get_bool().get(out.enable_reading) != simdjson::SUCCESS) {
    error = "apiRestrictions without ipRestrict, enableFutures, enableWithdrawals or enableReading";
    return Status::ParseError;
  }
  return Status::Ok;
}

Status RestClient::depth(std::string_view symbol, int limit, std::string& body,
                         std::string& error) {
  RestResponse r;
  const Status s = json_call("GET", "/fapi/v1/depth",
                             {{"symbol", std::string{symbol}}, {"limit", std::to_string(limit)}},
                             Security::None, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::open_orders(std::string& body, std::string& error) {
  RestResponse r;
  const Status s = json_call("GET", "/fapi/v1/openOrders", {}, Security::Signed, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::query_order(std::string_view symbol, std::string_view client_order_id,
                               std::string& body, bool& found, std::string& error) {
  found = false;
  RestResponse r;
  const Status s =
      call("GET", "/fapi/v1/order",
           {{"symbol", std::string{symbol}}, {"origClientOrderId", std::string{client_order_id}}},
           Security::Signed, r, error);
  if (!core::ok(s)) {
    return s;
  }
  if (r.status == 400 && r.error_code == -2013) {
    return Status::Ok; // "Order does not exist."
  }
  if (r.status != 200) {
    error = "/fapi/v1/order: HTTP " + std::to_string(r.status) +
            (r.error_code != 0 ? " code " + std::to_string(r.error_code) + " " + r.error_msg
                               : std::string{});
    return Status::InvalidArgument;
  }
  found = true;
  body = std::move(r.body);
  return Status::Ok;
}

Status RestClient::balances(std::string& body, std::string& error) {
  RestResponse r;
  const Status s = json_call("GET", "/fapi/v3/balance", {}, Security::Signed, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::positions(std::string& body, std::string& error) {
  RestResponse r;
  const Status s = json_call("GET", "/fapi/v3/positionRisk", {}, Security::Signed, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::user_trades(std::string_view symbol, std::optional<std::uint64_t> from_id,
                               std::int64_t start_ms, int limit, std::string& body,
                               std::string& error) {
  Params params{{"symbol", std::string{symbol}}, {"limit", std::to_string(limit)}};
  if (from_id) {
    params.emplace_back("fromId", std::to_string(*from_id));
  } else {
    params.emplace_back("startTime", std::to_string(start_ms));
  }
  RestResponse r;
  const Status s =
      json_call("GET", "/fapi/v1/userTrades", std::move(params), Security::Signed, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::order_trades(std::string_view symbol, std::uint64_t order_id, std::string& body,
                                std::string& error) {
  Params params{{"symbol", std::string{symbol}}, {"orderId", std::to_string(order_id)}};
  RestResponse r;
  const Status s =
      json_call("GET", "/fapi/v1/userTrades", std::move(params), Security::Signed, r, error);
  if (core::ok(s)) {
    body = std::move(r.body);
  }
  return s;
}

Status RestClient::order_call(std::string_view method, RequestKind kind, const Params& order,
                              PlaceAck& ack, RequestError& refusal, bool& refused,
                              std::string& error) {
  refused = false;
  RestResponse r;
  const Status s = call(method, "/fapi/v1/order", order, Security::Signed, r, error);
  if (!core::ok(s)) {
    return s; // the request may or may not have reached the venue: in flight
  }
  if (r.status == 200) {
    return decode_order_result(r.body, ack, error);
  }
  if (r.status >= 400 && r.status < 500 && r.error_code != 0) {
    refused = true;
    refusal = RequestError{
        kind, param(order, kind == RequestKind::Place ? "newClientOrderId" : "origClientOrderId"),
        r.error_code, r.error_msg, static_cast<std::uint64_t>(now_ms())};
    return Status::Ok;
  }
  // 5xx or an unknown body: the outcome is unknown (Binance: "treat as unknown, not failed").
  error = "HTTP " + std::to_string(r.status) + ": the order's outcome is unknown";
  return Status::IoError;
}

Status RestClient::place(const Params& order, PlaceAck& ack, RequestError& refusal, bool& refused,
                         std::string& error) {
  return order_call("POST", RequestKind::Place, order, ack, refusal, refused, error);
}
Status RestClient::modify(const Params& order, PlaceAck& ack, RequestError& refusal, bool& refused,
                          std::string& error) {
  return order_call("PUT", RequestKind::Modify, order, ack, refusal, refused, error);
}
Status RestClient::cancel(const Params& order, PlaceAck& ack, RequestError& refusal, bool& refused,
                          std::string& error) {
  return order_call("DELETE", RequestKind::Cancel, order, ack, refusal, refused, error);
}

} // namespace jarvis::adapter::binance
