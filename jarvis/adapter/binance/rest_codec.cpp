#include "jarvis/adapter/binance/rest_codec.hpp"

#include <charconv>
#include <optional>
#include <vector>

#include <simdjson.h>

#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/order_events.hpp"

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

// ---- reconciliation ----------------------------------------------------------------------------

namespace {

core::UnixNanos ms_to_ns(std::uint64_t ms) { return core::UnixNanos{ms * 1'000'000}; }

Status parse_json(std::string_view json, dom::parser& parser, simdjson::padded_string& padded,
                  dom::element& doc, std::string_view what, std::string& error) {
  padded = simdjson::padded_string{json};
  if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
    error = std::string{what} + " is not JSON";
    return Status::ParseError;
  }
  return Status::Ok;
}

std::string_view text_of(const dom::element& e, std::string_view key) {
  std::string_view v;
  return e[key].get_string().get(v) == simdjson::SUCCESS ? v : std::string_view{};
}

std::uint64_t uint_of(const dom::element& e, std::string_view key) {
  std::uint64_t v = 0;
  return e[key].get_uint64().get(v) == simdjson::SUCCESS ? v : 0;
}

bool bool_of(const dom::element& e, std::string_view key) {
  bool v = false;
  return e[key].get_bool().get(v) == simdjson::SUCCESS && v;
}

template <typename Id> bool id_of(std::string_view text, Id& out) {
  return core::ok(Id::from(text, out));
}

template <typename Id> Id decimal_id(std::uint64_t v) {
  Id out;
  static_cast<void>(Id::from(std::to_string(v), out));
  return out;
}

std::optional<model::OrderStatus> status_of(std::string_view s) {
  if (s == "NEW") {
    return model::OrderStatus::Accepted;
  }
  if (s == "PARTIALLY_FILLED") {
    return model::OrderStatus::PartiallyFilled;
  }
  if (s == "FILLED") {
    return model::OrderStatus::Filled;
  }
  if (s == "CANCELED") {
    return model::OrderStatus::Canceled;
  }
  if (s == "REJECTED") {
    return model::OrderStatus::Rejected;
  }
  if (s == "EXPIRED" || s == "EXPIRED_IN_MATCH") {
    return model::OrderStatus::Expired;
  }
  return std::nullopt;
}

model::OrderType type_of(std::string_view t) {
  if (t == "MARKET") {
    return model::OrderType::Market;
  }
  if (t == "STOP") {
    return model::OrderType::StopLimit;
  }
  if (t == "STOP_MARKET") {
    return model::OrderType::StopMarket;
  }
  if (t == "TAKE_PROFIT") {
    return model::OrderType::LimitIfTouched;
  }
  if (t == "TAKE_PROFIT_MARKET") {
    return model::OrderType::MarketIfTouched;
  }
  if (t == "TRAILING_STOP_MARKET") {
    return model::OrderType::TrailingStopMarket;
  }
  return model::OrderType::Limit;
}

model::TimeInForce tif_of(std::string_view t) {
  if (t == "IOC") {
    return model::TimeInForce::Ioc;
  }
  if (t == "FOK") {
    return model::TimeInForce::Fok;
  }
  if (t == "GTD") {
    return model::TimeInForce::Gtd;
  }
  return model::TimeInForce::Gtc; // GTC, and GTX (post-only)
}

Status order_report(const dom::element& o, const ReportContext& ctx,
                    std::vector<model::OrderStatusReport>& out, std::size_t& skipped,
                    std::string& error) {
  const std::optional<std::uint32_t> index = ctx.symbols->find(text_of(o, "symbol"));
  if (!index) {
    ++skipped;
    return Status::Ok;
  }
  const SymbolEntry& sym = (*ctx.symbols)[*index];
  const std::optional<model::OrderStatus> status = status_of(text_of(o, "status"));
  if (!status) {
    error = "an order with an unknown status " + std::string{text_of(o, "status")};
    return Status::ParseError;
  }
  model::OrderStatusReport r;
  r.account_id = ctx.account_id;
  r.instrument_id = sym.id;
  model::ClientOrderId cid;
  if (id_of(text_of(o, "clientOrderId"), cid)) {
    r.client_order_id = cid;
  }
  r.venue_order_id = decimal_id<model::VenueOrderId>(uint_of(o, "orderId"));
  r.order_side = text_of(o, "side") == "SELL" ? model::OrderSide::Sell : model::OrderSide::Buy;
  r.order_type = type_of(text_of(o, "type"));
  r.time_in_force = tif_of(text_of(o, "timeInForce"));
  r.post_only = text_of(o, "timeInForce") == "GTX";
  r.reduce_only = bool_of(o, "reduceOnly");
  r.order_status = *status;
  if (!core::ok(exact_quantity(text_of(o, "origQty"), sym.size_precision, r.quantity)) ||
      !core::ok(exact_quantity(text_of(o, "executedQty"), sym.size_precision, r.filled_qty))) {
    error = "an order's quantity off the instrument's grid";
    return Status::PrecisionLoss;
  }
  model::Price px;
  if (core::ok(exact_price(text_of(o, "price"), sym.price_precision, px)) && px.raw() > 0) {
    r.price = px;
  }
  r.ts_accepted = ms_to_ns(uint_of(o, "time"));
  r.ts_last = ms_to_ns(uint_of(o, "updateTime"));
  r.ts_init = ctx.recv;
  out.push_back(r);
  return Status::Ok;
}

} // namespace

Status decode_order_reports(std::string_view json, const ReportContext& ctx,
                            std::vector<model::OrderStatusReport>& out, std::size_t& skipped,
                            std::string& error) {
  dom::parser parser;
  simdjson::padded_string padded;
  dom::element doc;
  if (const Status s = parse_json(json, parser, padded, doc, "an order answer", error);
      !core::ok(s)) {
    return s;
  }
  dom::array list;
  if (doc.get_array().get(list) != simdjson::SUCCESS) {
    return order_report(doc, ctx, out, skipped, error);
  }
  for (const dom::element o : list) {
    if (const Status s = order_report(o, ctx, out, skipped, error); !core::ok(s)) {
      return s;
    }
  }
  return Status::Ok;
}

Status decode_balances(std::string_view json, std::vector<model::AccountBalance>& out,
                       std::size_t& skipped, std::string& error) {
  dom::parser parser;
  simdjson::padded_string padded;
  dom::element doc;
  if (const Status s = parse_json(json, parser, padded, doc, "the balances", error); !core::ok(s)) {
    return s;
  }
  dom::array list;
  if (doc.get_array().get(list) != simdjson::SUCCESS) {
    error = "the balances are not a list";
    return Status::ParseError;
  }
  for (const dom::element b : list) {
    const std::string asset{text_of(b, "asset")};
    model::Money total;
    model::Money available;
    if (!core::ok(model::Money::parse(std::string{text_of(b, "balance")} + " " + asset, total)) ||
        !core::ok(model::Money::parse(std::string{text_of(b, "availableBalance")} + " " + asset,
                                      available))) {
      ++skipped; // not a built-in currency
      continue;
    }
    const model::Money& free = available.raw() < total.raw() ? available : total;
    model::Money locked;
    model::AccountBalance balance;
    if (!core::ok(model::Money::from_raw(total.raw() - free.raw(), total.currency(), locked)) ||
        !core::ok(model::AccountBalance::create(total, locked, free, balance))) {
      error = "a balance of " + asset + " out of range";
      return Status::OutOfRange;
    }
    out.push_back(balance);
  }
  return Status::Ok;
}

Status decode_positions(std::string_view json, const ReportContext& ctx,
                        std::vector<model::PositionStatusReport>& out, std::size_t& skipped,
                        std::string& error) {
  dom::parser parser;
  simdjson::padded_string padded;
  dom::element doc;
  if (const Status s = parse_json(json, parser, padded, doc, "the positions", error);
      !core::ok(s)) {
    return s;
  }
  dom::array list;
  if (doc.get_array().get(list) != simdjson::SUCCESS) {
    error = "the positions are not a list";
    return Status::ParseError;
  }
  for (const dom::element p : list) {
    const std::optional<std::uint32_t> index = ctx.symbols->find(text_of(p, "symbol"));
    if (!index) {
      ++skipped;
      continue;
    }
    const SymbolEntry& sym = (*ctx.symbols)[*index];
    std::string_view amount = text_of(p, "positionAmt");
    const bool short_side = !amount.empty() && amount.front() == '-';
    if (short_side) {
      amount.remove_prefix(1);
    }
    model::PositionStatusReport r;
    if (!core::ok(exact_quantity(amount, sym.size_precision, r.quantity))) {
      error = "a position off the instrument's grid";
      return Status::PrecisionLoss;
    }
    if (r.quantity.raw() == 0) {
      continue;
    }
    r.account_id = ctx.account_id;
    r.instrument_id = sym.id;
    r.position_side = short_side ? model::PositionSide::Short : model::PositionSide::Long;
    model::Price entry;
    if (core::ok(model::Price::parse(text_of(p, "entryPrice"), entry)) && entry.raw() > 0) {
      r.avg_px_open = entry;
    }
    r.ts_last = ms_to_ns(uint_of(p, "updateTime"));
    r.ts_init = ctx.recv;
    out.push_back(r);
  }
  return Status::Ok;
}

Status decode_user_trades(std::string_view json, const ReportContext& ctx,
                          std::vector<model::FillReport>& out, std::uint64_t& last_id,
                          std::size_t& count, std::string& error) {
  count = 0;
  dom::parser parser;
  simdjson::padded_string padded;
  dom::element doc;
  if (const Status s = parse_json(json, parser, padded, doc, "the trades", error); !core::ok(s)) {
    return s;
  }
  dom::array list;
  if (doc.get_array().get(list) != simdjson::SUCCESS) {
    error = "the trades are not a list";
    return Status::ParseError;
  }
  for (const dom::element t : list) {
    ++count;
    const std::uint64_t id = uint_of(t, "id");
    last_id = id > last_id ? id : last_id;
    const std::optional<std::uint32_t> index = ctx.symbols->find(text_of(t, "symbol"));
    if (!index) {
      continue;
    }
    const SymbolEntry& sym = (*ctx.symbols)[*index];
    model::FillReport f;
    f.account_id = ctx.account_id;
    f.instrument_id = sym.id;
    f.venue_order_id = decimal_id<model::VenueOrderId>(uint_of(t, "orderId"));
    f.trade_id = decimal_id<model::TradeId>(id);
    f.order_side = text_of(t, "side") == "SELL" ? model::OrderSide::Sell : model::OrderSide::Buy;
    if (!core::ok(exact_quantity(text_of(t, "qty"), sym.size_precision, f.last_qty)) ||
        !core::ok(exact_price(text_of(t, "price"), sym.price_precision, f.last_px))) {
      error = "a trade off the instrument's grid";
      return Status::PrecisionLoss;
    }
    if (!core::ok(model::Money::parse(std::string{text_of(t, "commission")} + " " +
                                          std::string{text_of(t, "commissionAsset")},
                                      f.commission)) &&
        !core::ok(model::Money::from_raw(0, sym.settlement, f.commission))) {
      error = "a trade whose commission cannot be read";
      return Status::ParseError;
    }
    f.liquidity_side =
        bool_of(t, "maker") ? model::LiquiditySide::Maker : model::LiquiditySide::Taker;
    f.ts_event = ms_to_ns(uint_of(t, "time"));
    f.ts_init = ctx.recv;
    out.push_back(f);
  }
  return Status::Ok;
}

} // namespace jarvis::adapter::binance
