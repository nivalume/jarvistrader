// The user data stream decoder and the order tracker (jarvis/adapter/binance/user_stream.hpp,
// order_tracker.hpp): Binance reports, in the documented formats, become the kernel's order and
// account events, with fills deduplicated across TRADE_LITE and ORDER_TRADE_UPDATE.

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/adapter/binance/exchange_info.hpp"
#include "jarvis/adapter/binance/order_tracker.hpp"
#include "jarvis/adapter/binance/user_stream.hpp"
#include "jarvis/adapter/codec.hpp"
#include "jarvis/model/account.hpp"
#include "jarvis/model/order_events.hpp"

namespace {

namespace adapter = jarvis::adapter;
namespace binance = jarvis::adapter::binance;
namespace model = jarvis::model;
using jarvis::core::Status;
using jarvis::core::UnixNanos;

template <typename Id> Id make(std::string_view text) {
  Id id;
  REQUIRE(Id::from(text, id) == Status::Ok);
  return id;
}

adapter::SymbolTable table() {
  adapter::SymbolTable t;
  model::InstrumentId id;
  REQUIRE(binance::perpetual_id("BTCUSDT", id) == Status::Ok);
  model::Currency usdt;
  REQUIRE(model::Currency::builtin("USDT", usdt) == Status::Ok);
  REQUIRE(t.add("BTCUSDT", adapter::SymbolEntry{id, 1, 3, usdt}) == Status::Ok);
  return t;
}

binance::VenueIdentity identity() {
  binance::VenueIdentity v;
  v.trader_id = make<model::TraderId>("jarvis-001");
  v.account_id = make<model::AccountId>("BINANCE-001");
  v.strategies = {make<model::StrategyId>("mm-001")};
  v.seed = 7;
  return v;
}

model::SubmitOrder submit(std::string_view cid) {
  model::SubmitOrder c;
  c.client_order_id = make<model::ClientOrderId>(cid);
  REQUIRE(binance::perpetual_id("BTCUSDT", c.instrument_id) == Status::Ok);
  c.order_side = model::OrderSide::Buy;
  c.order_type = model::OrderType::Limit;
  return c;
}

binance::UserReport report(std::string_view json) {
  binance::UserReport r;
  std::string error;
  REQUIRE_MESSAGE(binance::decode_user_report(json, r, error) == Status::Ok, error);
  return r;
}

std::string order_update(std::string_view cid, std::string_view x, std::uint64_t trade,
                         std::uint64_t order_time, std::string_view extra = "") {
  return std::string{
             R"({"e":"ORDER_TRADE_UPDATE","E":1790000000100,"T":1790000000099,"o":{"s":"BTCUSDT","c":")"} +
         std::string{cid} +
         R"(","S":"BUY","o":"LIMIT","f":"GTC","q":"0.010","p":"65000.0","ap":"65000.0","sp":"0","x":")" +
         std::string{x} +
         R"(","X":"PARTIALLY_FILLED","i":8886774,"l":"0.004","z":"0.004","L":"65000.0","N":"USDT","n":"0.052","T":)" +
         std::to_string(order_time) + R"(,"t":)" + std::to_string(trade) +
         R"(,"b":"0","a":"0","m":true,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0")" +
         std::string{extra} + "}}";
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("user data stream reports decode, in bare and combined form") {
    const auto o =
        std::get<binance::OrderReport>(report(order_update("C-1", "TRADE", 42, 1790000000090)));
    CHECK(o.symbol == "BTCUSDT");
    CHECK(o.execution_type == "TRADE");
    CHECK(o.order_id == 8886774);
    CHECK(o.trade_id == 42);
    CHECK(o.maker);
    CHECK(o.commission == "0.052");
    CHECK(o.order_time_ms == 1790000000090ULL);
    CHECK(o.transaction_time_ms == 1790000000099ULL);

    const auto lite = std::get<binance::TradeLiteReport>(report(
        R"({"stream":"k","data":{"e":"TRADE_LITE","E":1721895408092,"T":1721895408214,"s":"BTCUSDT","q":"0.001","p":"0","m":false,"c":"z8hc","S":"BUY","L":"64089.20","l":"0.040","t":109100866,"i":8886774}})"));
    CHECK(lite.client_order_id == "z8hc");
    CHECK(lite.last_qty == "0.040");
    CHECK(lite.trade_id == 109100866);

    const auto acct = std::get<binance::AccountReport>(report(
        R"({"e":"ACCOUNT_UPDATE","E":1564745798939,"T":1564745798938,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","wb":"122624.12345678","cw":"100.12345678","bc":"50.12345678"}],"P":[{"s":"BTCUSDT","pa":"0.010","ep":"65000.0","bep":"0","cr":"200","up":"1.0","mt":"cross","iw":"0","ps":"BOTH"}]}})"));
    CHECK(acct.reason == "FUNDING_FEE");
    REQUIRE(acct.balances.size() == 1);
    CHECK(acct.balances[0].wallet == "122624.12345678");
    REQUIRE(acct.positions.size() == 1);
    CHECK(acct.positions[0].amount == "0.010");

    const auto cfg = std::get<binance::ConfigReport>(report(
        R"({"e":"ACCOUNT_CONFIG_UPDATE","E":1611646737479,"T":1611646737476,"ac":{"s":"BTCUSDT","l":25}})"));
    CHECK(cfg.leverage == 25U);
    const auto expired = std::get<binance::ListenKeyExpiredReport>(
        report(R"({"e":"listenKeyExpired","E":"1736996475556","listenKey":"WsCMN0a4"})"));
    CHECK(expired.event_time_ms == 1736996475556ULL); // sent as a string
    CHECK(std::holds_alternative<binance::MarginCallReport>(report(
        R"({"e":"MARGIN_CALL","E":1587727187525,"cw":"3.16812045","p":[{"s":"ETHUSDT","ps":"LONG","pa":"1.327","mt":"CROSSED","iw":"0","mp":"187.17127","up":"-1.166074","mm":"1.614445"}]})")));

    binance::UserReport r;
    std::string error;
    CHECK(binance::decode_user_report(R"({"e":"SOMETHING_NEW"})", r, error) ==
          Status::UnsupportedMessage);
    CHECK(binance::decode_user_report("nope", r, error) == Status::ParseError);
    CHECK(binance::decode_user_report(R"({"e":"ORDER_TRADE_UPDATE","E":1})", r, error) ==
          Status::ParseError);
  }

  TEST_CASE("an order's life: acknowledgement, Lite fill, its commission, cancel") {
    const adapter::SymbolTable t = table();
    binance::OrderTracker tracker{t, identity()};
    adapter::CollectingEmitter out;
    tracker.on_submit(submit("C-1"));

    REQUIRE(tracker.on_place_ack({"C-1", 8886774, "NEW", 1790000000050}, UnixNanos{1}, out) ==
            Status::Ok);
    REQUIRE(out.events.size() == 1);
    const auto& acc = std::get<model::OrderAccepted>(out.events[0]);
    CHECK(acc.venue_order_id.view() == "8886774");
    CHECK(acc.header.strategy_id.view() == "mm-001");
    CHECK(acc.header.trader_id.view() == "jarvis-001");
    CHECK(acc.header.ts_event == UnixNanos{1790000000050ULL * 1'000'000});
    CHECK(acc.header.ts_init == UnixNanos{1});

    // The stream's NEW arrives after the acknowledgement: nothing new.
    REQUIRE(tracker.on_report(report(order_update("C-1", "NEW", 0, 1790000000050)), UnixNanos{2},
                              out) == Status::Ok);
    CHECK(out.events.size() == 1);

    // TRADE_LITE first: the fill, without commission.
    REQUIRE(
        tracker.on_report(
            report(
                R"({"e":"TRADE_LITE","E":1790000000095,"T":1790000000094,"s":"BTCUSDT","q":"0.010","p":"65000.0","m":true,"c":"C-1","S":"BUY","L":"65000.0","l":"0.004","t":42,"i":8886774})"),
            UnixNanos{3}, out) == Status::Ok);
    REQUIRE(out.events.size() == 2);
    const auto& lite = std::get<model::OrderFilled>(out.events[1]);
    CHECK(lite.trade_id.view() == "42");
    CHECK(lite.last_qty.raw() == 4'000'000);
    CHECK(lite.last_px.raw() == 65000'000'000'000LL);
    CHECK(lite.liquidity_side == model::LiquiditySide::Maker);
    CHECK_FALSE(lite.commission.has_value());
    CHECK((lite.info_flags & static_cast<std::uint8_t>(model::FillInfo::Lite)) != 0);
    CHECK(lite.currency.code() == "USDT");

    // ORDER_TRADE_UPDATE of the same trade: passed on once, with the commission.
    REQUIRE(tracker.on_report(report(order_update("C-1", "TRADE", 42, 1790000000090)), UnixNanos{4},
                              out) == Status::Ok);
    REQUIRE(out.events.size() == 3);
    const auto& full = std::get<model::OrderFilled>(out.events[2]);
    CHECK(full.trade_id.view() == "42");
    CHECK(full.commission.value_or(model::Money{}).raw() == 52'000'000); // 0.052 USDT
    CHECK((full.info_flags & static_cast<std::uint8_t>(model::FillInfo::Lite)) == 0);
    // The same report again changes nothing.
    REQUIRE(tracker.on_report(report(order_update("C-1", "TRADE", 42, 1790000000090)), UnixNanos{5},
                              out) == Status::Ok);
    CHECK(out.events.size() == 3);

    // A report older than the order's last update is stale.
    REQUIRE(tracker.on_report(report(order_update("C-1", "TRADE", 43, 1790000000010)), UnixNanos{6},
                              out) == Status::Ok);
    CHECK(out.events.size() == 3);

    // A liquidation fill, then the cancel (once).
    REQUIRE(tracker.on_report(report(order_update("C-1", "CALCULATED", 44, 1790000000200)),
                              UnixNanos{7}, out) == Status::Ok);
    const auto& liq = std::get<model::OrderFilled>(out.events.at(3));
    CHECK((liq.info_flags & static_cast<std::uint8_t>(model::FillInfo::Liquidation)) != 0);
    REQUIRE(tracker.on_report(report(order_update("C-1", "CANCELED", 0, 1790000000300)),
                              UnixNanos{8}, out) == Status::Ok);
    REQUIRE(tracker.on_report(report(order_update("C-1", "CANCELED", 0, 1790000000300)),
                              UnixNanos{9}, out) == Status::Ok);
    REQUIRE(out.events.size() == 5);
    CHECK(std::holds_alternative<model::OrderCanceled>(out.events[4]));

    // Someone else's order.
    REQUIRE(tracker.on_report(report(order_update("OTHER", "NEW", 0, 1)), UnixNanos{10}, out) ==
            Status::Ok);
    CHECK(out.events.size() == 5);

    const binance::TrackerStats& st = tracker.stats();
    CHECK(st.lite_fills == 1);
    CHECK(st.late_commissions == 1);
    CHECK(st.duplicate_trades == 1);
    CHECK(st.stale_reports == 1);
    CHECK(st.unknown_orders == 1);
    // Event ids differ and derive from the seed.
    CHECK_FALSE(model::header_of(std::get<model::OrderAccepted>(out.events[0])).event_id ==
                model::header_of(std::get<model::OrderFilled>(out.events[1])).event_id);
  }

  TEST_CASE("refused requests and amendments become the kernel's events") {
    const adapter::SymbolTable t = table();
    binance::OrderTracker tracker{t, identity()};
    adapter::CollectingEmitter out;
    tracker.on_submit(submit("C-2"));
    REQUIRE(tracker.on_request_error({binance::RequestKind::Place, "C-2", -5022,
                                      "Due to the order could not be executed as maker", 5},
                                     UnixNanos{1}, out) == Status::Ok);
    const auto& rej = std::get<model::OrderRejected>(out.events.at(0));
    CHECK(rej.due_post_only);
    CHECK(rej.reason.view().starts_with("BINANCE_-5022 Due to"));

    tracker.on_submit(submit("C-3"));
    REQUIRE(tracker.on_place_ack({"C-3", 99, "NEW", 6}, UnixNanos{2}, out) == Status::Ok);
    REQUIRE(tracker.on_request_error(
                {binance::RequestKind::Modify, "C-3", -5027, "No need to modify", 7}, UnixNanos{3},
                out) == Status::Ok);
    REQUIRE(tracker.on_request_error(
                {binance::RequestKind::Cancel, "C-3", -2011, "Unknown order sent.", 8},
                UnixNanos{4}, out) == Status::Ok);
    CHECK(std::get<model::OrderModifyRejected>(out.events.at(2))
              .venue_order_id.value_or(model::VenueOrderId{})
              .view() == "99");
    CHECK(std::get<model::OrderCancelRejected>(out.events.at(3)).reason.view() ==
          "BINANCE_-2011 Unknown order sent.");
    REQUIRE(tracker.on_report(report(order_update("C-3", "AMENDMENT", 0, 9, "")), UnixNanos{5},
                              out) == Status::Ok);
    const auto& upd = std::get<model::OrderUpdated>(out.events.at(4));
    CHECK(upd.quantity.raw() == 10'000'000);
    CHECK(upd.price.value_or(model::Price{}).raw() == 65000'000'000'000LL);
  }

  TEST_CASE("account updates merge into the full balance table") {
    const adapter::SymbolTable t = table();
    binance::OrderTracker tracker{t, identity()};
    adapter::CollectingEmitter out;
    REQUIRE(
        tracker.on_report(
            report(
                R"({"e":"ACCOUNT_UPDATE","E":10,"T":9,"a":{"m":"ORDER","B":[{"a":"USDT","wb":"1000.5","cw":"1000.5","bc":"0"}],"P":[]}})"),
            UnixNanos{1}, out) == Status::Ok);
    REQUIRE(
        tracker.on_report(
            report(
                R"({"e":"ACCOUNT_UPDATE","E":11,"T":10,"a":{"m":"DEPOSIT","B":[{"a":"BNB","wb":"2","cw":"2","bc":"2"}],"P":[]}})"),
            UnixNanos{2}, out) == Status::Ok);
    REQUIRE(out.events.size() == 2);
    const auto& state = std::get<model::AccountState>(out.events[1]);
    REQUIRE(state.balances.size() == 2); // BNB and USDT, both
    CHECK(state.balances[0].currency().code() == "BNB");
    CHECK(state.balances[1].total.raw() == 1000'500'000'000LL);
    CHECK(state.is_reported);
    CHECK(state.ts_event == UnixNanos{11'000'000});
  }

  TEST_CASE("reconciliation: unclosed orders, next trade ids, and absorbing a snapshot") {
    const adapter::SymbolTable symbols = table();
    binance::OrderTracker tracker{symbols, identity()};
    tracker.on_submit(submit("C-1"));
    tracker.on_submit(submit("C-2"));
    tracker.on_submit(submit("C-3"));
    adapter::CollectingEmitter out;
    REQUIRE(tracker.on_report(report(order_update("C-1", "TRADE", 42, 1790000000090)), UnixNanos{1},
                              out) == Status::Ok);
    std::vector<binance::TrackedOrder> open = tracker.unclosed();
    REQUIRE(open.size() == 3);
    CHECK(open[0].client_order_id == "C-1");
    CHECK(open[0].symbol == "BTCUSDT");
    CHECK(open[0].venue_order_id == 8886774);
    CHECK(open[1].venue_order_id == 0);
    CHECK(tracker.next_trades().at("BTCUSDT") == 43);

    // The snapshot: C-2 filled while away (trade 50), C-3 canceled.
    std::vector<model::OrderStatusReport> orders(2);
    orders[0].client_order_id = make<model::ClientOrderId>("C-2");
    orders[0].venue_order_id = make<model::VenueOrderId>("900");
    orders[0].order_status = model::OrderStatus::Filled;
    orders[0].ts_last = UnixNanos{1790000000500'000'000};
    orders[1].client_order_id = make<model::ClientOrderId>("C-3");
    orders[1].venue_order_id = make<model::VenueOrderId>("901");
    orders[1].order_status = model::OrderStatus::Canceled;
    std::vector<model::FillReport> fills(1);
    fills[0].client_order_id = orders[0].client_order_id;
    fills[0].venue_order_id = orders[0].venue_order_id;
    fills[0].trade_id = make<model::TradeId>("50");
    model::VenueSnapshot snap;
    snap.orders = orders;
    snap.fills = fills;
    tracker.absorb(snap);
    open = tracker.unclosed();
    REQUIRE(open.size() == 1);
    CHECK(open[0].client_order_id == "C-1");
    CHECK(tracker.next_trades().at("BTCUSDT") == 51);

    // A late report of the trade the snapshot showed is not passed on again.
    out.clear();
    REQUIRE(tracker.on_report(report(order_update("C-2", "TRADE", 50, 1790000000400)), UnixNanos{2},
                              out) == Status::Ok);
    CHECK(out.events.empty());
  }
}
