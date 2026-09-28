#include "jarvis/adapter/binance/order_tracker.hpp"

#include <algorithm>
#include <charconv>

#include "jarvis/model/account.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/uuid.hpp"

namespace jarvis::adapter::binance {

namespace {

using core::Status;

core::UnixNanos from_ms(std::uint64_t ms) { return core::UnixNanos{ms * 1'000'000}; }

std::string decimal(std::uint64_t v) {
  std::array<char, 24> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return {buf.data(), static_cast<std::size_t>(end - buf.data())};
}

template <typename Id> Id id_of(std::string_view text) {
  Id id;
  static_cast<void>(Id::from(text, id));
  return id;
}

model::ReasonText reason_of(int code, std::string_view message) {
  std::string text = "BINANCE_" + std::to_string(code);
  if (!message.empty()) {
    text += ' ';
    text += message;
  }
  text.resize(std::min(text.size(), model::ReasonText::capacity()));
  model::ReasonText out;
  static_cast<void>(model::ReasonText::from(text, out));
  return out;
}

} // namespace

void OrderTracker::on_submit(const model::SubmitOrder& c) {
  Order o;
  o.strategy = c.strategy_index;
  o.symbol = symbols_->find(c.instrument_id).value_or(0);
  o.instrument_id = c.instrument_id;
  o.client_order_id = c.client_order_id;
  o.side = c.order_side;
  o.type = c.order_type;
  orders_.insert_or_assign(std::string{c.client_order_id.view()}, std::move(o));
}

std::vector<TrackedOrder> OrderTracker::unclosed() const {
  std::vector<TrackedOrder> out;
  for (const auto& [cid, o] : orders_) {
    if (o.closed) {
      continue;
    }
    std::uint64_t venue = 0;
    static_cast<void>(std::from_chars(o.venue_order_id.data(),
                                      o.venue_order_id.data() + o.venue_order_id.size(), venue));
    out.push_back(TrackedOrder{symbols_->venue_symbol(o.symbol), cid, venue});
  }
  std::sort(out.begin(), out.end(), [](const TrackedOrder& a, const TrackedOrder& b) {
    return a.client_order_id < b.client_order_id;
  });
  return out;
}

std::map<std::string, std::uint64_t> OrderTracker::next_trades() const {
  std::map<std::string, std::uint64_t> out;
  for (const auto& [cid, o] : orders_) {
    if (o.trades.empty()) {
      continue;
    }
    std::uint64_t& next = out[symbols_->venue_symbol(o.symbol)];
    next = std::max(next, o.trades.rbegin()->first + 1);
  }
  return out;
}

void OrderTracker::absorb(const model::VenueSnapshot& snapshot) {
  for (const model::OrderStatusReport& r : snapshot.orders) {
    if (!r.client_order_id) {
      continue;
    }
    const auto it = orders_.find(std::string{r.client_order_id->view()});
    if (it == orders_.end()) {
      continue;
    }
    Order& o = it->second;
    if (o.venue_order_id.empty()) {
      o.venue_order_id.assign(r.venue_order_id.view());
    }
    o.accepted = o.accepted || r.order_status != model::OrderStatus::Rejected;
    const bool open = r.order_status == model::OrderStatus::Accepted ||
                      r.order_status == model::OrderStatus::PartiallyFilled ||
                      r.order_status == model::OrderStatus::Triggered;
    if (!open) {
      o.closed = true;
    }
    o.last_update_ms = std::max(o.last_update_ms, r.ts_last.value() / 1'000'000);
  }
  for (const model::FillReport& f : snapshot.fills) {
    if (!f.client_order_id) {
      continue;
    }
    const auto it = orders_.find(std::string{f.client_order_id->view()});
    std::uint64_t trade = 0;
    const std::string_view text = f.trade_id.view();
    if (it == orders_.end() ||
        std::from_chars(text.data(), text.data() + text.size(), trade).ec != std::errc{}) {
      continue;
    }
    it->second.trades[trade] = TradeState::Full;
  }
}

OrderTracker::Order* OrderTracker::find(std::string_view client_order_id) {
  const auto it = orders_.find(std::string{client_order_id});
  if (it == orders_.end()) {
    ++stats_.unknown_orders;
    return nullptr;
  }
  return &it->second;
}

model::OrderEventHeader OrderTracker::header(const Order& o, std::uint64_t ts_ms,
                                             core::UnixNanos recv) {
  model::OrderEventHeader h;
  h.trader_id = id_.trader_id;
  if (o.strategy < id_.strategies.size()) {
    h.strategy_id = id_.strategies[o.strategy];
  }
  h.instrument_id = o.instrument_id;
  h.client_order_id = o.client_order_id;
  h.event_id = model::Uuid4::derive(rng_, ++serial_, 0);
  h.ts_event = ts_ms == 0 ? recv : from_ms(ts_ms);
  h.ts_init = recv;
  return h;
}

Status OrderTracker::accepted(Order& o, std::uint64_t ts_ms, core::UnixNanos recv,
                              EventEmitter& out) {
  if (o.accepted || o.closed) {
    ++stats_.ignored;
    return Status::Ok;
  }
  o.accepted = true;
  model::OrderAccepted e;
  e.header = header(o, ts_ms, recv);
  e.venue_order_id = id_of<model::VenueOrderId>(o.venue_order_id);
  e.account_id = id_.account_id;
  return out.event(model::Event{e});
}

Status OrderTracker::on_place_ack(const PlaceAck& a, core::UnixNanos recv, EventEmitter& out) {
  Order* o = find(a.client_order_id);
  if (o == nullptr) {
    return Status::Ok;
  }
  if (o->venue_order_id.empty()) {
    o->venue_order_id = decimal(a.order_id);
  }
  // A terminal answer (an IOC that filled or expired at once) is left to the stream, which
  // reports NEW and then the outcome.
  if (a.status == "NEW" || a.status == "PARTIALLY_FILLED") {
    return accepted(*o, a.update_time_ms, recv, out);
  }
  return Status::Ok;
}

Status OrderTracker::on_request_error(const RequestError& e, core::UnixNanos recv,
                                      EventEmitter& out) {
  Order* o = find(e.client_order_id);
  if (o == nullptr) {
    return Status::Ok;
  }
  const model::ReasonText reason = reason_of(e.code, e.message);
  const std::optional<model::VenueOrderId> vid =
      o->venue_order_id.empty()
          ? std::nullopt
          : std::optional<model::VenueOrderId>{id_of<model::VenueOrderId>(o->venue_order_id)};
  switch (e.kind) {
  case RequestKind::Place: {
    o->closed = true;
    model::OrderRejected r;
    r.header = header(*o, e.time_ms, recv);
    r.account_id = id_.account_id;
    r.reason = reason;
    r.due_post_only = e.code == -5022; // post-only would have taken liquidity
    return out.event(model::Event{r});
  }
  case RequestKind::Modify: {
    model::OrderModifyRejected r;
    r.header = header(*o, e.time_ms, recv);
    r.reason = reason;
    r.venue_order_id = vid;
    r.account_id = id_.account_id;
    return out.event(model::Event{r});
  }
  case RequestKind::Cancel: {
    model::OrderCancelRejected r;
    r.header = header(*o, e.time_ms, recv);
    r.reason = reason;
    r.venue_order_id = vid;
    r.account_id = id_.account_id;
    return out.event(model::Event{r});
  }
  }
  return Status::Ok;
}

Status OrderTracker::fill(Order& o, std::uint64_t trade_id, std::string_view qty,
                          std::string_view px, bool maker, bool lite, bool liquidation,
                          std::string_view commission, std::string_view commission_asset,
                          std::uint64_t ts_ms, core::UnixNanos recv, EventEmitter& out) {
  const auto it = o.trades.find(trade_id);
  const bool late = it != o.trades.end() && it->second == TradeState::Lite && !lite;
  if (it != o.trades.end() && !late) {
    ++stats_.duplicate_trades;
    return Status::Ok;
  }
  const SymbolEntry& sym = (*symbols_)[o.symbol];
  model::OrderFilled f;
  f.header = header(o, ts_ms, recv);
  f.venue_order_id = id_of<model::VenueOrderId>(o.venue_order_id);
  f.account_id = id_.account_id;
  f.trade_id = id_of<model::TradeId>(decimal(trade_id));
  f.order_side = o.side;
  f.order_type = o.type;
  if (!core::ok(exact_quantity(qty, sym.size_precision, f.last_qty)) ||
      !core::ok(exact_price(px, sym.price_precision, f.last_px))) {
    return fail(Status::PrecisionLoss, "a fill off the instrument's grid");
  }
  f.liquidity_side = maker ? model::LiquiditySide::Maker : model::LiquiditySide::Taker;
  f.currency = sym.settlement;
  if (!lite && !commission.empty()) {
    model::Money fee;
    if (core::ok(model::Money::parse(std::string{commission} + " " + std::string{commission_asset},
                                     fee))) {
      f.commission = fee;
      if (f.currency.code().empty()) {
        f.currency = fee.currency();
      }
    }
  }
  if (lite) {
    f.info_flags |= static_cast<std::uint8_t>(model::FillInfo::Lite);
    ++stats_.lite_fills;
  }
  if (liquidation) {
    f.info_flags |= static_cast<std::uint8_t>(model::FillInfo::Liquidation);
  }
  if (late) {
    ++stats_.late_commissions;
  }
  o.trades[trade_id] = lite ? TradeState::Lite : TradeState::Full;
  return out.event(model::Event{f});
}

Status OrderTracker::order_report(const OrderReport& r, core::UnixNanos recv, EventEmitter& out) {
  Order* o = find(r.client_order_id);
  if (o == nullptr) {
    return Status::Ok;
  }
  if (r.order_time_ms < o->last_update_ms) {
    ++stats_.stale_reports;
    return Status::Ok;
  }
  o->last_update_ms = r.order_time_ms;
  if (o->venue_order_id.empty()) {
    o->venue_order_id = decimal(r.order_id);
  }
  const std::string& x = r.execution_type;
  if (x == "NEW") {
    return accepted(*o, r.transaction_time_ms, recv, out);
  }
  if (x == "TRADE" || x == "CALCULATED") {
    return fill(*o, r.trade_id, r.last_qty, r.last_price, r.maker, false, x == "CALCULATED",
                r.commission, r.commission_asset, r.order_time_ms, recv, out);
  }
  if ((x == "CANCELED" || x == "EXPIRED") && !o->closed) {
    o->closed = true;
    const model::VenueOrderId vid = id_of<model::VenueOrderId>(o->venue_order_id);
    if (x == "CANCELED") {
      model::OrderCanceled e;
      e.header = header(*o, r.transaction_time_ms, recv);
      e.venue_order_id = vid;
      e.account_id = id_.account_id;
      return out.event(model::Event{e});
    }
    model::OrderExpired e;
    e.header = header(*o, r.transaction_time_ms, recv);
    e.venue_order_id = vid;
    e.account_id = id_.account_id;
    return out.event(model::Event{e});
  }
  if (x == "AMENDMENT") {
    const SymbolEntry& sym = (*symbols_)[o->symbol];
    model::OrderUpdated e;
    e.header = header(*o, r.transaction_time_ms, recv);
    e.venue_order_id = id_of<model::VenueOrderId>(o->venue_order_id);
    e.account_id = id_.account_id;
    model::Price px;
    if (!core::ok(exact_quantity(r.orig_qty, sym.size_precision, e.quantity)) ||
        !core::ok(exact_price(r.price, sym.price_precision, px))) {
      return fail(Status::PrecisionLoss, "an amendment off the instrument's grid");
    }
    e.price = px;
    return out.event(model::Event{e});
  }
  ++stats_.ignored;
  return Status::Ok;
}

Status OrderTracker::account_report(const AccountReport& r, core::UnixNanos recv,
                                    EventEmitter& out) {
  for (const BalanceReport& b : r.balances) {
    wallet_[b.asset] = b.wallet;
  }
  ++stats_.account_updates;
  balances_.clear();
  for (const auto& [asset, wallet] : wallet_) {
    model::Money total;
    model::Money zero;
    model::AccountBalance balance;
    std::string text = wallet;
    text += ' ';
    text += asset;
    if (!core::ok(model::Money::parse(text, total)) ||
        !core::ok(model::Money::from_raw(0, total.currency(), zero)) ||
        !core::ok(model::AccountBalance::create(total, zero, total, balance))) {
      continue; // an asset nautilus has no currency for
    }
    balances_.push_back(balance);
  }
  model::AccountState state;
  state.account_id = id_.account_id;
  state.account_type = model::AccountType::Margin;
  state.balances = balances_; // borrowed until the next account update
  state.is_reported = true;
  state.event_id = model::Uuid4::derive(rng_, ++serial_, 0);
  state.ts_event = r.event_time_ms == 0 ? recv : from_ms(r.event_time_ms);
  state.ts_init = recv;
  return out.event(model::Event{state});
}

Status OrderTracker::on_report(const UserReport& r, core::UnixNanos recv, EventEmitter& out) {
  if (const auto* o = std::get_if<OrderReport>(&r)) {
    return order_report(*o, recv, out);
  }
  if (const auto* t = std::get_if<TradeLiteReport>(&r)) {
    Order* o = find(t->client_order_id);
    if (o == nullptr) {
      return Status::Ok;
    }
    if (o->venue_order_id.empty()) {
      o->venue_order_id = decimal(t->order_id);
    }
    return fill(*o, t->trade_id, t->last_qty, t->last_price, t->maker, true, false, {}, {},
                t->transaction_time_ms, recv, out);
  }
  if (const auto* a = std::get_if<AccountReport>(&r)) {
    return account_report(*a, recv, out);
  }
  // MARGIN_CALL, ACCOUNT_CONFIG_UPDATE and listenKeyExpired are the connection manager's and
  // the monitors' (M5); nothing for the order state here.
  ++stats_.ignored;
  return Status::Ok;
}

} // namespace jarvis::adapter::binance
