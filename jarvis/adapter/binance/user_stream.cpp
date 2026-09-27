#include "jarvis/adapter/binance/user_stream.hpp"

#include <charconv>

#include <simdjson.h>

namespace jarvis::adapter::binance {

namespace {

namespace dom = simdjson::dom;
using core::Status;

// Field readers: a missing optional field leaves the default.
std::string text(const dom::element& e, std::string_view key) {
  std::string_view v;
  return e[key].get_string().get(v) == simdjson::SUCCESS ? std::string{v} : std::string{};
}

// Integers, also when sent as strings ("E" of listenKeyExpired is one).
std::uint64_t number(const dom::element& e, std::string_view key) {
  std::uint64_t v = 0;
  if (e[key].get_uint64().get(v) == simdjson::SUCCESS) {
    return v;
  }
  std::string_view s;
  if (e[key].get_string().get(s) == simdjson::SUCCESS) {
    static_cast<void>(std::from_chars(s.data(), s.data() + s.size(), v));
  }
  return v;
}

bool flag(const dom::element& e, std::string_view key) {
  bool v = false;
  return e[key].get_bool().get(v) == simdjson::SUCCESS && v;
}

std::vector<PositionReport> positions_of(const dom::element& e, std::string_view key) {
  std::vector<PositionReport> out;
  dom::array list;
  if (e[key].get_array().get(list) != simdjson::SUCCESS) {
    return out;
  }
  for (const dom::element p : list) {
    out.push_back(PositionReport{text(p, "s"), text(p, "pa"), text(p, "ep"), text(p, "up"),
                                 text(p, "mt"), text(p, "ps")});
  }
  return out;
}

Status order_report(const dom::element& ev, UserReport& out, std::string& error) {
  dom::element o;
  if (ev["o"].get(o) != simdjson::SUCCESS) {
    error = "ORDER_TRADE_UPDATE without o";
    return Status::ParseError;
  }
  OrderReport r;
  r.symbol = text(o, "s");
  r.client_order_id = text(o, "c");
  r.side = text(o, "S");
  r.order_type = text(o, "o");
  r.time_in_force = text(o, "f");
  r.execution_type = text(o, "x");
  r.order_status = text(o, "X");
  r.order_id = number(o, "i");
  r.orig_qty = text(o, "q");
  r.price = text(o, "p");
  r.last_qty = text(o, "l");
  r.last_price = text(o, "L");
  r.cum_qty = text(o, "z");
  r.commission = text(o, "n");
  r.commission_asset = text(o, "N");
  r.trade_id = number(o, "t");
  r.maker = flag(o, "m");
  r.realized_profit = text(o, "rp");
  r.order_time_ms = number(o, "T");
  r.event_time_ms = number(ev, "E");
  r.transaction_time_ms = number(ev, "T");
  if (r.symbol.empty() || r.client_order_id.empty() || r.execution_type.empty()) {
    error = "ORDER_TRADE_UPDATE without s, c or x";
    return Status::ParseError;
  }
  out = std::move(r);
  return Status::Ok;
}

Status trade_lite(const dom::element& ev, UserReport& out, std::string& error) {
  TradeLiteReport r{text(ev, "s"),   text(ev, "c"),   text(ev, "S"),   text(ev, "l"),
                    text(ev, "L"),   number(ev, "t"), number(ev, "i"), flag(ev, "m"),
                    number(ev, "E"), number(ev, "T")};
  if (r.symbol.empty() || r.client_order_id.empty() || r.last_qty.empty()) {
    error = "TRADE_LITE without s, c or l";
    return Status::ParseError;
  }
  out = std::move(r);
  return Status::Ok;
}

Status account_update(const dom::element& ev, UserReport& out, std::string& error) {
  dom::element a;
  if (ev["a"].get(a) != simdjson::SUCCESS) {
    error = "ACCOUNT_UPDATE without a";
    return Status::ParseError;
  }
  AccountReport r;
  r.reason = text(a, "m");
  dom::array balances;
  if (a["B"].get_array().get(balances) == simdjson::SUCCESS) {
    for (const dom::element b : balances) {
      r.balances.push_back(
          BalanceReport{text(b, "a"), text(b, "wb"), text(b, "cw"), text(b, "bc")});
    }
  }
  r.positions = positions_of(a, "P");
  r.event_time_ms = number(ev, "E");
  r.transaction_time_ms = number(ev, "T");
  out = std::move(r);
  return Status::Ok;
}

Status config_update(const dom::element& ev, UserReport& out) {
  ConfigReport r;
  r.event_time_ms = number(ev, "E");
  dom::element ac;
  if (ev["ac"].get(ac) == simdjson::SUCCESS) {
    r.symbol = text(ac, "s");
    std::uint64_t l = 0;
    if (ac["l"].get_uint64().get(l) == simdjson::SUCCESS) {
      r.leverage = static_cast<std::uint32_t>(l);
    }
  }
  dom::element ai;
  bool j = false;
  if (ev["ai"].get(ai) == simdjson::SUCCESS && ai["j"].get_bool().get(j) == simdjson::SUCCESS) {
    r.multi_assets = j;
  }
  out = std::move(r);
  return Status::Ok;
}

} // namespace

Status decode_user_report(std::string_view json, UserReport& out, std::string& error) {
  dom::parser parser;
  dom::element doc;
  const simdjson::padded_string padded{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = "not JSON";
    return Status::ParseError;
  }
  dom::element ev = doc;
  dom::element data;
  if (doc["data"].get(data) == simdjson::SUCCESS) {
    ev = data; // combined-stream envelope
  }
  const std::string type = text(ev, "e");
  if (type == "ORDER_TRADE_UPDATE") {
    return order_report(ev, out, error);
  }
  if (type == "TRADE_LITE") {
    return trade_lite(ev, out, error);
  }
  if (type == "ACCOUNT_UPDATE") {
    return account_update(ev, out, error);
  }
  if (type == "MARGIN_CALL") {
    out = MarginCallReport{text(ev, "cw"), positions_of(ev, "p"), number(ev, "E")};
    return Status::Ok;
  }
  if (type == "ACCOUNT_CONFIG_UPDATE") {
    return config_update(ev, out);
  }
  if (type == "listenKeyExpired") {
    out = ListenKeyExpiredReport{text(ev, "listenKey"), number(ev, "E")};
    return Status::Ok;
  }
  error = "unsupported user data event " + (type.empty() ? std::string{"(none)"} : type);
  return Status::UnsupportedMessage;
}

} // namespace jarvis::adapter::binance
