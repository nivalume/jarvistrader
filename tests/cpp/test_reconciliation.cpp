#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/clock.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/engine.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/execution/reconciliation.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/model/instruments.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/model/outputs.hpp"
#include "jarvis/model/reports.hpp"
#include "jarvis/strategy/context.hpp"
#include "jarvis/strategy/strategy_set.hpp"
#include "jarvis/testkit/property.hpp"

// Reconciliation in the kernel (jarvis/execution/reconciliation.hpp): unit cases for each step,
// and a property test that runs random venue histories against the invariants of
// specs/tla/Reconciliation.tla.

namespace {

namespace ex = jarvis::execution;
namespace md = jarvis::model;
namespace st = jarvis::strategy;
using jarvis::core::EventKey;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::model::LifecycleReason;
using jarvis::model::NodeState;
using jarvis::testkit::Gen;

constexpr std::string_view kBtc = "BTCUSDT-PERP.BINANCE";

template <typename Id> Id make_id(std::string_view text) {
  Id out;
  REQUIRE(Id::from(text, out) == Status::Ok);
  return out;
}
md::InstrumentId iid(std::string_view text) {
  md::InstrumentId id;
  REQUIRE(md::InstrumentId::parse(text, id) == Status::Ok);
  return id;
}
md::Price price(std::string_view text) {
  md::Price p;
  REQUIRE(md::Price::parse(text, p) == Status::Ok);
  return p;
}
md::Quantity quantity(std::string_view text) {
  md::Quantity q;
  REQUIRE(md::Quantity::parse(text, q) == Status::Ok);
  return q;
}
md::Quantity units(std::uint64_t n) { // n x 1.000
  md::Quantity q;
  REQUIRE(md::Quantity::from_raw(n * 1'000'000'000ULL, 3, q) == Status::Ok);
  return q;
}
md::Currency usdt() {
  md::Currency c;
  REQUIRE(md::Currency::builtin("USDT", c) == Status::Ok);
  return c;
}
md::Money money(std::string_view text) {
  md::Money m;
  REQUIRE(md::Money::parse(std::string{text} + " USDT", m) == Status::Ok);
  return m;
}

md::Event perpetual_definition(std::uint64_t ts) {
  md::CryptoPerpetual p;
  md::InstrumentCommon& c = p.common;
  c.id = iid(kBtc);
  c.raw_symbol = make_id<md::Symbol>("BTCUSDT");
  md::Currency btc;
  REQUIRE(md::Currency::builtin("BTC", btc) == Status::Ok);
  c.base_currency = btc;
  c.quote_currency = usdt();
  c.settlement_currency = usdt();
  c.price_increment = price("0.1");
  c.price_precision = 1;
  c.size_increment = quantity("0.001");
  c.size_precision = 3;
  c.multiplier = quantity("1");
  REQUIRE(md::Decimal::parse("0.05", c.margin_init) == Status::Ok);
  REQUIRE(md::Decimal::parse("0.025", c.margin_maint) == Status::Ok);
  c.ts_event = UnixNanos{ts};
  c.ts_init = UnixNanos{ts};
  return md::Event{p};
}

md::Event lifecycle(NodeState from, NodeState to, std::uint64_t ts) {
  return md::Event{md::NodeLifecycle{from, to, LifecycleReason::Synced, UnixNanos{ts}}};
}
md::Event running(std::uint64_t ts) {
  return lifecycle(NodeState::Syncing, NodeState::Running, ts);
}
md::Event stream(bool up, std::uint64_t ts) {
  md::ConnectionStatus c;
  c.kind = md::ConnectionKind::UserStream;
  c.up = up;
  c.ts_init = UnixNanos{ts};
  return md::Event{c};
}

template <typename E>
E venue_event(std::uint64_t ts_event, std::uint64_t ts_init, const md::ClientOrderId& id) {
  E e;
  e.header.instrument_id = iid(kBtc);
  e.header.client_order_id = id;
  e.header.ts_event = UnixNanos{ts_event};
  e.header.ts_init = UnixNanos{ts_init};
  return e;
}
md::Event accepted(std::uint64_t ts, const md::ClientOrderId& id, std::string_view venue_id,
                   std::uint64_t ts_init = 0) {
  auto e = venue_event<md::OrderAccepted>(ts, ts_init == 0 ? ts : ts_init, id);
  e.venue_order_id = make_id<md::VenueOrderId>(venue_id);
  return md::Event{e};
}
md::Event filled(std::uint64_t ts, const md::ClientOrderId& id, std::string_view trade,
                 std::string_view qty, std::string_view px, std::uint64_t ts_init = 0) {
  auto e = venue_event<md::OrderFilled>(ts, ts_init == 0 ? ts : ts_init, id);
  e.trade_id = make_id<md::TradeId>(trade);
  e.last_qty = quantity(qty);
  e.last_px = price(px);
  return md::Event{e};
}
md::Event updated(std::uint64_t ts, const md::ClientOrderId& id, std::string_view qty,
                  std::string_view px, std::uint64_t ts_init = 0) {
  auto e = venue_event<md::OrderUpdated>(ts, ts_init == 0 ? ts : ts_init, id);
  e.quantity = quantity(qty);
  e.price = price(px);
  return md::Event{e};
}

std::string order_line(const md::OrderEvent& e) {
  ex::OrderEventKind kind{};
  REQUIRE(ex::kind_of(e, kind));
  std::string line{ex::to_string(kind)};
  if (const auto* r = std::get_if<md::OrderRejected>(&e)) {
    line += " ";
    line += r->reason.view();
  } else if (const auto* c = std::get_if<md::OrderCanceled>(&e); c != nullptr && c->reason) {
    line += " ";
    line += c->reason->view();
  }
  return line;
}

// Submits `orders` limit buys of `qty` at 100.0 on start; logs its order events.
// Test strategies write through pointers to state owned by the test.
// NOLINTBEGIN(readability-make-member-function-const,readability-convert-member-functions-to-static)
struct Trader {
  std::size_t orders = 0;
  std::string_view qty = "1.000";
  std::vector<md::ClientOrderId>* ids = nullptr;
  std::vector<std::string>* log = nullptr;
  std::vector<std::string>* reconciled = nullptr;

  Status on_start(st::Context& ctx) {
    for (std::size_t i = 0; i < orders; ++i) {
      md::ClientOrderId id;
      REQUIRE(ctx.submit(ctx.limit(iid(kBtc), md::OrderSide::Buy, quantity(qty), price("100.0")),
                         id) == Status::Ok);
      ids->push_back(id);
    }
    return Status::Ok;
  }
  Status on_order_event(st::Context& /*ctx*/, const md::OrderEvent& e) {
    log->push_back(order_line(e));
    return Status::Ok;
  }
  Status on_reconciled(st::Context& ctx, const md::ReconcileOutcome& o) {
    reconciled->push_back("fills=" + std::to_string(o.fills) +
                          " state=" + std::string{md::to_string(ctx.trading_state())});
    return Status::Ok;
  }
};
// NOLINTEND(readability-make-member-function-const,readability-convert-member-functions-to-static)

st::KernelConfig config() {
  st::KernelConfig c;
  c.instruments = 8;
  c.strategies = 2;
  c.timers = 16;
  c.features = 4;
  c.bar_types = 4;
  c.buffers = 16;
  c.book_window_levels = 256;
  c.book_overflow_levels = 64;
  c.trading.orders = 64;
  c.trading.trades = 1024;
  c.trading.risk.orders_per_10s = 0;
  c.trading.risk.orders_per_minute = 0;
  return c;
}

using TestEngine = jarvis::engine::Engine<st::StaticStrategySet<Trader>>;

class Harness {
public:
  explicit Harness(std::size_t orders, std::string_view qty = "1.000",
                   st::KernelConfig c = config())
      : set_{Trader{orders, qty, &ids, &log, &reconciled}}, engine_{c, set_} {}

  TestEngine& engine() { return engine_; }
  const st::Trading& trading() { return engine_.kernel().trading; }
  const ex::Reconciler& reconciler() { return engine_.kernel().trading.reconciler; }

  Status step(const md::Event& event) {
    const Status s = engine_.step(EventKey{md::ts_init_of(event), 0, ++seq_}, event);
    for (const md::Output& o : engine_.outputs()) {
      outputs.push_back(o);
    }
    engine_.clear_outputs();
    return s;
  }
  void run(const std::vector<md::Event>& events) {
    for (const md::Event& e : events) {
      REQUIRE(step(e) == Status::Ok);
    }
  }

  const ex::OrderRecord& order(std::size_t i) {
    const std::uint32_t index = trading().oms.find(ids.at(i));
    REQUIRE(index != ex::kNoIndex);
    return trading().oms.at(index);
  }
  std::int64_t venue_position() { return trading().portfolio.venue(0).signed_raw(); }
  std::int64_t strategy_position() { return trading().portfolio.position(0, 0).signed_raw(); }

  template <typename T> std::vector<T> outputs_of() const {
    std::vector<T> out;
    for (const md::Output& o : outputs) {
      if (const auto* t = std::get_if<T>(&o)) {
        out.push_back(*t);
      }
    }
    return out;
  }

  std::vector<md::ClientOrderId> ids;
  std::vector<std::string> log;
  std::vector<std::string> reconciled; // on_reconciled calls
  std::vector<md::Output> outputs;

private:
  st::StaticStrategySet<Trader> set_;
  TestEngine engine_;
  std::uint64_t seq_ = 0;
};

// Storage a VenueSnapshot event borrows.
struct Snapshot {
  std::uint64_t ts = 0;
  std::vector<md::AccountBalance> balances;
  std::vector<md::OrderStatusReport> orders;
  std::vector<md::FillReport> fills;
  std::vector<md::PositionStatusReport> positions;

  md::Event event(std::uint64_t ts_init) const {
    md::VenueSnapshot s;
    s.account_id = make_id<md::AccountId>("SIM-001");
    s.ts_snapshot = UnixNanos{ts};
    s.balances = balances;
    s.orders = orders;
    s.fills = fills;
    s.positions = positions;
    s.ts_init = UnixNanos{ts_init};
    return md::Event{s};
  }

  md::OrderStatusReport& order(const std::optional<md::ClientOrderId>& id,
                               std::string_view venue_id, md::OrderStatus status,
                               std::string_view qty, std::string_view filled,
                               std::uint64_t ts_last) {
    md::OrderStatusReport r;
    r.instrument_id = iid(kBtc);
    r.client_order_id = id;
    r.venue_order_id = make_id<md::VenueOrderId>(venue_id);
    r.order_status = status;
    r.quantity = quantity(qty);
    r.filled_qty = quantity(filled);
    r.price = price("100.0");
    r.ts_accepted = UnixNanos{1};
    r.ts_last = UnixNanos{ts_last};
    orders.push_back(r);
    return orders.back();
  }
  void fill(const md::ClientOrderId& id, std::string_view venue_id, std::string_view trade,
            std::string_view qty, std::string_view px, std::uint64_t ts_event) {
    md::FillReport f;
    f.instrument_id = iid(kBtc);
    f.venue_order_id = make_id<md::VenueOrderId>(venue_id);
    f.trade_id = make_id<md::TradeId>(trade);
    f.last_qty = quantity(qty);
    f.last_px = price(px);
    f.commission = money("0.01");
    f.liquidity_side = md::LiquiditySide::Maker;
    f.client_order_id = id;
    f.ts_event = UnixNanos{ts_event};
    fills.push_back(f);
  }
  void position(md::PositionSide side, std::string_view qty, std::optional<std::string_view> avg) {
    md::PositionStatusReport p;
    p.instrument_id = iid(kBtc);
    p.position_side = side;
    p.quantity = quantity(qty);
    if (avg) {
      p.avg_px_open = price(*avg);
    }
    positions.push_back(p);
  }
  void balance(std::string_view total) {
    md::AccountBalance b;
    const md::Money zero = money("0");
    REQUIRE(md::AccountBalance::create(money(total), zero, money(total), b) == Status::Ok);
    balances.push_back(b);
  }
};

md::Event account(std::string_view total, std::uint64_t ts_event, std::uint64_t ts_init,
                  std::vector<md::AccountBalance>& storage) {
  md::AccountBalance b;
  REQUIRE(md::AccountBalance::create(money(total), money("0"), money(total), b) == Status::Ok);
  storage = {b};
  md::AccountState state;
  state.account_id = make_id<md::AccountId>("SIM-001");
  state.balances = storage;
  state.ts_event = UnixNanos{ts_event};
  state.ts_init = UnixNanos{ts_init};
  return md::Event{state};
}

// Starts the harness: the definition, 10000 USDT, Running (the strategy submits), then `acks` of
// its orders accepted as v0, v1, ...
void start(Harness& h, std::size_t acks) {
  std::vector<md::AccountBalance> storage;
  h.run({perpetual_definition(1), account("10000", 1, 1, storage), running(2)});
  for (std::size_t i = 0; i < acks; ++i) {
    h.run({accepted(3, h.ids.at(i), "v" + std::to_string(i))});
  }
  h.log.clear();
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("without a user stream the session is local and venue events apply at once") {
    Harness h{1};
    start(h, 1);
    CHECK(h.reconciler().phase() == ex::SyncPhase::Local);
    h.run({filled(4, h.ids[0], "t1", "0.400", "100.0")});
    CHECK(h.log == std::vector<std::string>{"FILLED"});
    CHECK(h.order(0).state.filled() == quantity("0.400"));
  }

  TEST_CASE("the stream dropping halts trading and holds venue events until the snapshot") {
    Harness h{3};
    start(h, 3);
    CHECK(h.trading().risk.trading_state() == md::TradingState::Active);
    h.run({stream(false, 10)});
    CHECK(h.reconciler().phase() == ex::SyncPhase::Disconnected);
    CHECK(h.trading().risk.trading_state() == md::TradingState::Halted);
    h.run({stream(true, 11), filled(20, h.ids[0], "t2", "0.300", "100.0", 12)});
    CHECK(h.reconciler().phase() == ex::SyncPhase::Buffering);
    CHECK(h.reconciler().held() == 1);
    CHECK(h.log.empty());
    CHECK(h.order(0).state.filled() == quantity("0.000"));

    Snapshot snap;
    snap.ts = 15;
    snap.order(h.ids[0], "v0", md::OrderStatus::PartiallyFilled, "1.000", "0.400", 12);
    snap.fill(h.ids[0], "v0", "t1", "0.400", "100.0", 12);
    snap.order(h.ids[1], "v1", md::OrderStatus::Canceled, "1.000", "0.000", 13);
    // h.ids[2] is missing: the venue does not know it.
    snap.order(make_id<md::ClientOrderId>("web_abc"), "v9", md::OrderStatus::Accepted, "2.000",
               "0.500", 14);
    snap.position(md::PositionSide::Long, "0.400", "100.0");
    snap.balance("9999.99"); // the fill report's commission
    h.run({snap.event(16)});

    CHECK(h.reconciler().phase() == ex::SyncPhase::Synced);
    CHECK(h.reconciler().held() == 0);
    CHECK(h.log == std::vector<std::string>{"FILLED", "CANCELED", "CANCELED LOST", "FILLED"});
    CHECK(h.order(0).state.filled() == quantity("0.700"));
    CHECK(h.order(0).state.status() == md::OrderStatus::PartiallyFilled);
    CHECK(h.order(1).state.status() == md::OrderStatus::Canceled);
    CHECK(h.order(2).state.status() == md::OrderStatus::Canceled);
    CHECK(h.venue_position() == 700'000'000);
    CHECK(h.strategy_position() == 700'000'000);

    const auto diffs = h.outputs_of<md::ReconciliationDiff>();
    REQUIRE(diffs.size() == 2);
    CHECK(diffs[0].kind == md::ReconcileDiffKind::LostOrder);
    CHECK(diffs[0].client_order_id == h.ids[2]);
    CHECK(diffs[0].local_raw == 1'000'000'000);
    CHECK(diffs[1].kind == md::ReconcileDiffKind::ExternalOrder);
    CHECK(diffs[1].venue_raw == 1'500'000'000);
    CHECK(h.outputs_of<md::CancelOrder>().empty()); // foreign orders are only reported
    const auto outcomes = h.outputs_of<md::ReconcileOutcome>();
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0].ts_snapshot == UnixNanos{15});
    CHECK(outcomes[0].orders == 3);
    CHECK(outcomes[0].fills == 1);
    CHECK(outcomes[0].closed == 1);
    CHECK(outcomes[0].lost == 1);
    CHECK(outcomes[0].external == 1);
    CHECK(outcomes[0].diffs == 2);
    CHECK(outcomes[0].buffered == 1);

    // Strategies are told, after the synthesized events; trading stays halted until the node
    // enters Running.
    CHECK(h.reconciled == std::vector<std::string>{"fills=1 state=HALTED"});
    CHECK(h.trading().risk.trading_state() == md::TradingState::Halted);
    h.run({running(17)});
    CHECK(h.trading().risk.trading_state() == md::TradingState::Active);
  }

  TEST_CASE("a fill both reported and held counts once") {
    Harness h{1};
    start(h, 1);
    h.run({stream(false, 10), stream(true, 11), filled(12, h.ids[0], "t1", "0.400", "100.0"),
           filled(16, h.ids[0], "t1", "0.400", "100.0")});
    Snapshot snap;
    snap.ts = 14;
    snap.order(h.ids[0], "v0", md::OrderStatus::PartiallyFilled, "1.000", "0.400", 12);
    snap.fill(h.ids[0], "v0", "t1", "0.400", "100.0", 12);
    snap.position(md::PositionSide::Long, "0.400", "100.0");
    h.run({snap.event(17)});
    CHECK(h.log == std::vector<std::string>{"FILLED"});
    CHECK(h.order(0).state.filled() == quantity("0.400"));
    CHECK(h.venue_position() == 400'000'000);
    CHECK(h.strategy_position() == 400'000'000);
    CHECK(h.reconciler().stats().stale == 1); // the copy at 12 is in the snapshot
    CHECK(h.trading().stats.duplicate_fills == 1);
    CHECK(h.outputs_of<md::ReconciliationDiff>().empty());
  }

  TEST_CASE("held events are applied oldest first, and a late status older than T_s is stale") {
    Harness h{1};
    start(h, 1);
    h.run({stream(false, 10), stream(true, 11),
           updated(30, h.ids[0], "3.000", "100.0", 12), // arrives before the older update
           updated(25, h.ids[0], "2.000", "100.0", 13)});
    Snapshot snap;
    snap.ts = 20;
    snap.order(h.ids[0], "v0", md::OrderStatus::Accepted, "1.000", "0.000", 3);
    h.run({snap.event(21)});
    CHECK(h.log == std::vector<std::string>{"UPDATED", "UPDATED"});
    CHECK(h.order(0).state.quantity() == quantity("3.000"));
    h.run({running(22), updated(18, h.ids[0], "5.000", "100.0", 23)}); // older than T_s
    CHECK(h.order(0).state.quantity() == quantity("3.000"));
    CHECK(h.trading().stats.stale_order_events == 1);
  }

  TEST_CASE("an order the venue has but the node saw only as submitted is accepted first") {
    Harness h{1};
    start(h, 0);
    h.run({stream(false, 10), stream(true, 11)});
    Snapshot snap;
    snap.ts = 20;
    snap.order(h.ids[0], "v0", md::OrderStatus::PartiallyFilled, "1.000", "0.250", 15);
    snap.fill(h.ids[0], "v0", "t1", "0.250", "100.0", 15);
    snap.position(md::PositionSide::Long, "0.250", "100.0");
    h.run({snap.event(21)});
    CHECK(h.log == std::vector<std::string>{"ACCEPTED", "FILLED"});
    CHECK(h.order(0).venue_order_id == make_id<md::VenueOrderId>("v0"));
    CHECK(h.order(0).state.status() == md::OrderStatus::PartiallyFilled);
  }

  TEST_CASE("an unacknowledged order is lost only after the grace period") {
    Harness h{2};
    start(h, 0); // submitted at 2, never acknowledged
    h.run({stream(false, 10), stream(true, 11)});
    Snapshot early;
    early.ts = 4'000'000'000; // 4 s after the submit: may still be in flight
    h.run({early.event(4'000'000'001)});
    CHECK(h.log.empty());
    CHECK(h.order(0).state.status() == md::OrderStatus::Submitted);

    h.run({stream(false, 5'000'000'000), stream(true, 5'000'000'001)});
    Snapshot late;
    late.ts = 6'000'000'000;
    late.order(h.ids[1], "v1", md::OrderStatus::Accepted, "1.000", "0.000", 5);
    h.run({late.event(6'000'000'001)});
    CHECK(h.log == std::vector<std::string>{"ACCEPTED", "REJECTED LOST"}); // step 1, then 3
    CHECK(h.order(0).state.status() == md::OrderStatus::Rejected);
    CHECK(h.order(1).state.status() == md::OrderStatus::Accepted);
  }

  TEST_CASE("a filled order whose fills were not all reported closes as canceled") {
    Harness h{1};
    start(h, 1);
    h.run({stream(false, 10), stream(true, 11)});
    Snapshot snap;
    snap.ts = 20;
    snap.order(h.ids[0], "v0", md::OrderStatus::Filled, "1.000", "1.000", 15);
    snap.fill(h.ids[0], "v0", "t1", "0.600", "100.0", 14); // t2 (0.400) is missing
    snap.position(md::PositionSide::Long, "1.000", "100.0");
    h.run({snap.event(21)});
    CHECK(h.log == std::vector<std::string>{"FILLED", "CANCELED UNREPORTED_FILLS"});
    CHECK(h.order(0).state.status() == md::OrderStatus::Canceled);
    const auto diffs = h.outputs_of<md::ReconciliationDiff>();
    REQUIRE(diffs.size() == 2);
    CHECK(diffs[0].kind == md::ReconcileDiffKind::FilledQuantity);
    CHECK(diffs[0].local_raw == 600'000'000);
    CHECK(diffs[0].venue_raw == 1'000'000'000);
    CHECK(diffs[1].kind == md::ReconcileDiffKind::Position);
    CHECK(h.venue_position() == 1'000'000'000);  // set from the snapshot
    CHECK(h.strategy_position() == 600'000'000); // the strategy's share counts its own fills
  }

  TEST_CASE("the venue's other statuses close local orders") {
    Harness h{3};
    start(h, 3);
    h.run({stream(false, 10), stream(true, 11)});
    Snapshot snap;
    snap.ts = 20;
    snap.order(h.ids[0], "v0", md::OrderStatus::Expired, "1.000", "0.000", 15);
    snap.order(h.ids[1], "v1", md::OrderStatus::Rejected, "1.000", "0.000", 15);
    snap.order(h.ids[2], "v2", md::OrderStatus::Accepted, "2.000", "0.000", 16); // modified
    h.run({snap.event(21)});
    CHECK(h.log == std::vector<std::string>{"EXPIRED", "REJECTED VENUE", "UPDATED"});
    CHECK(h.order(2).state.quantity() == quantity("2.000"));
    CHECK(h.outputs_of<md::ReconcileOutcome>().at(0).closed == 2);
  }

  TEST_CASE("orders with this node's tag are canceled, foreign ones reported") {
    Harness h{1};
    start(h, 1);
    h.run({stream(false, 10), stream(true, 11)});
    Snapshot snap;
    snap.ts = 20;
    snap.order(h.ids[0], "v0", md::OrderStatus::Accepted, "1.000", "0.000", 3);
    snap.order(make_id<md::ClientOrderId>("jarvis-000000-00000007"), "v7",
               md::OrderStatus::Accepted, "1.000", "0.000", 5); // an earlier epoch
    snap.order(make_id<md::ClientOrderId>("web_1"), "v8", md::OrderStatus::Accepted, "1.000",
               "0.000", 5);
    snap.order(std::nullopt, "v9", md::OrderStatus::Accepted, "1.000", "0.000", 5);
    snap.order(make_id<md::ClientOrderId>("jarvis-000000-00000006"), "v6",
               md::OrderStatus::Canceled, "1.000", "0.000", 5); // closed: history
    h.run({snap.event(21)});
    const auto cancels = h.outputs_of<md::CancelOrder>();
    REQUIRE(cancels.size() == 1);
    CHECK(cancels[0].strategy_index == ex::kNoStrategy);
    CHECK(cancels[0].client_order_id.view() == "jarvis-000000-00000007");
    CHECK(cancels[0].venue_order_id == make_id<md::VenueOrderId>("v7"));
    CHECK(h.outputs_of<md::ReconcileOutcome>().at(0).external == 3);
    CHECK(h.outputs_of<md::ReconciliationDiff>().size() == 3);
  }

  TEST_CASE("positions and balances are set from the snapshot") {
    Harness h{0};
    start(h, 0);
    h.run({stream(false, 10), stream(true, 11)});
    Snapshot snap;
    snap.ts = 20;
    snap.position(md::PositionSide::Short, "1.500", "101.0");
    md::PositionStatusReport other;
    other.instrument_id = iid("ETHUSDT-PERP.BINANCE");
    other.position_side = md::PositionSide::Long;
    other.quantity = quantity("2.000");
    snap.positions.push_back(other);
    snap.balance("1000.00");
    h.run({snap.event(21)});
    CHECK(h.venue_position() == -1'500'000'000);
    md::Price avg;
    REQUIRE(h.trading().portfolio.venue(0).avg_px_open(avg));
    CHECK(avg == price("101.0"));
    std::int64_t wallet = 0;
    REQUIRE(h.trading().portfolio.wallet(usdt(), wallet));
    CHECK(wallet == money("1000.00").raw());
    const auto diffs = h.outputs_of<md::ReconciliationDiff>();
    REQUIRE(diffs.size() == 3);
    CHECK(diffs[0].kind == md::ReconcileDiffKind::Position);
    CHECK(diffs[0].venue_raw == -1'500'000'000);
    CHECK(diffs[1].kind == md::ReconcileDiffKind::Position); // not traded here: reported only
    CHECK(diffs[1].instrument_id == iid("ETHUSDT-PERP.BINANCE"));
    CHECK(diffs[2].kind == md::ReconcileDiffKind::Balance);
    CHECK(diffs[2].currency == usdt());
    CHECK(diffs[2].local_raw == money("10000").raw());
    CHECK(diffs[2].venue_raw == money("1000.00").raw());

    // The next time the venue lists no position, the instrument is flat there.
    h.run({stream(false, 30), stream(true, 31)});
    Snapshot flat;
    flat.ts = 40;
    h.run({flat.event(41)});
    CHECK(h.venue_position() == 0);
    REQUIRE(h.trading().portfolio.wallet(usdt(), wallet));
    CHECK(wallet == money("1000.00").raw()); // no balances listed: kept
  }

  TEST_CASE("a held account state newer than the snapshot replaces its balances") {
    Harness h{0};
    start(h, 0);
    const md::Money total = money("990.00");
    std::vector<md::AccountBalance> storage;
    h.run({stream(false, 10), stream(true, 11), account("990.00", 25, 12, storage)});
    Snapshot snap;
    snap.ts = 20;
    snap.balance("1000.00");
    h.run({snap.event(21)});
    std::int64_t wallet = 0;
    REQUIRE(h.trading().portfolio.wallet(usdt(), wallet));
    CHECK(wallet == total.raw());
    CHECK(h.outputs_of<md::ReconcileOutcome>().at(0).buffered == 1);
  }

  TEST_CASE("a snapshot while synced or disconnected is ignored, and the hold is bounded") {
    st::KernelConfig c = config();
    c.trading.reconcile.held = 2;
    Harness h{1, "1.000", c};
    start(h, 1);
    h.run({stream(true, 10)});
    Snapshot snap;
    snap.ts = 12;
    snap.order(h.ids[0], "v0", md::OrderStatus::Accepted, "1.000", "0.000", 3);
    h.run({snap.event(13), snap.event(14)});
    CHECK(h.reconciler().stats().reconciliations == 1);
    CHECK(h.reconciler().stats().ignored_snapshots == 1);
    CHECK(h.outputs_of<md::ReconcileOutcome>().size() == 1);
    h.run({stream(false, 15), snap.event(16)}); // asked for before the drop
    CHECK(h.reconciler().phase() == ex::SyncPhase::Disconnected);
    CHECK(h.reconciler().stats().ignored_snapshots == 2);

    h.run({stream(false, 20), stream(true, 21), filled(22, h.ids[0], "t1", "0.100", "100.0"),
           filled(23, h.ids[0], "t2", "0.100", "100.0")});
    CHECK(h.step(filled(24, h.ids[0], "t3", "0.100", "100.0")) == Status::CapacityExceeded);
  }
}

// ---- property: random venue histories --------------------------------------------------------

namespace {

// The order event inside an input event that holds one.
md::OrderEvent order_event(const md::Event& e) {
  md::OrderEvent out;
  std::visit(
      [&out](const auto& v) {
        if constexpr (jarvis::engine::detail::is_alternative_v<std::decay_t<decltype(v)>,
                                                               md::OrderEvent>) {
          out = v;
        }
      },
      e);
  return out;
}

// The venue of specs/tla/Reconciliation.tla: each order opens, fills one unit at a time and
// finishes (filled or canceled); every change takes the next venue time and emits a user stream
// message, lost while the stream is down. The network reorders and duplicates what is in flight.
class Venue {
public:
  enum class St : std::uint8_t { None, Open, Done };

  Venue(std::size_t orders, std::uint64_t max_fill) : max_fill_{max_fill}, orders_(orders) {}

  struct Order {
    St st = St::None;
    std::uint64_t fills = 0;
    std::uint64_t ts_open = 0;
    std::uint64_t ts_last = 0;
    std::vector<std::uint64_t> fill_ts;
  };

  // A pending message: order o's event at venue time t.
  struct Message {
    std::size_t order = 0;
    md::OrderEvent event;
  };

  [[nodiscard]] std::uint64_t time() const { return time_; }
  [[nodiscard]] const Order& order(std::size_t o) const { return orders_.at(o); }
  [[nodiscard]] std::size_t size() const { return orders_.size(); }
  [[nodiscard]] std::uint64_t max_fill() const { return max_fill_; }
  std::vector<Message>& chan() { return chan_; }

  bool up = false;

  // Tries a random change of order o; false when none applies.
  bool change(std::size_t o, Gen& g, const md::ClientOrderId& id) {
    Order& r = orders_.at(o);
    const std::string venue_id = "v" + std::to_string(o);
    if (r.st == St::None) {
      r.st = St::Open;
      r.ts_open = ++time_;
      r.ts_last = time_;
      emit(o, accepted(time_, id, venue_id));
      return true;
    }
    if (r.st != St::Open) {
      return false;
    }
    if (g.chance(1, 4)) {
      r.st = St::Done;
      r.ts_last = ++time_;
      auto e = venue_event<md::OrderCanceled>(time_, time_, id);
      emit(o, md::Event{e});
      return true;
    }
    ++r.fills;
    r.ts_last = ++time_;
    r.fill_ts.push_back(time_);
    if (r.fills == max_fill_) {
      r.st = St::Done;
    }
    emit(o, filled(time_, id, trade_id(o, r.fills), "1.000", "100.0"));
    return true;
  }

  static std::string trade_id(std::size_t o, std::uint64_t n) {
    return "o" + std::to_string(o) + "-" + std::to_string(n);
  }

  // The venue as of now (an atomic snapshot).
  Snapshot snapshot(const std::vector<md::ClientOrderId>& ids) const {
    Snapshot s;
    s.ts = time_;
    std::uint64_t total = 0;
    for (std::size_t o = 0; o < orders_.size(); ++o) {
      const Order& r = orders_[o];
      total += r.fills;
      if (r.st == St::None) {
        continue;
      }
      md::OrderStatus status = md::OrderStatus::Accepted;
      if (r.st == St::Open && r.fills > 0) {
        status = md::OrderStatus::PartiallyFilled;
      } else if (r.st == St::Done) {
        status = r.fills == max_fill_ ? md::OrderStatus::Filled : md::OrderStatus::Canceled;
      }
      md::OrderStatusReport& report =
          s.order(ids[o], "v" + std::to_string(o), status, "1.000", "0.000", r.ts_last);
      report.quantity = units(max_fill_);
      report.filled_qty = units(r.fills);
      report.ts_accepted = UnixNanos{r.ts_open};
      for (std::uint64_t n = 1; n <= r.fills; ++n) {
        s.fill(ids[o], "v" + std::to_string(o), trade_id(o, n), "1.000", "100.0", r.fill_ts[n - 1]);
      }
    }
    if (total > 0) {
      md::PositionStatusReport p;
      p.instrument_id = iid(kBtc);
      p.position_side = md::PositionSide::Long;
      p.quantity = units(total);
      p.avg_px_open = price("100.0");
      s.positions.push_back(p);
    }
    return s;
  }

private:
  void emit(std::size_t o, const md::Event& e) {
    if (up) {
      chan_.push_back(Message{o, order_event(e)});
    }
  }

  std::uint64_t max_fill_;
  std::uint64_t time_ = 0;
  std::vector<Order> orders_;
  std::vector<Message> chan_;
};

md::Event as_event(const md::OrderEvent& e) {
  return std::visit([](const auto& v) { return md::Event{v}; }, e);
}

// Re-stamps the input time of a message delivered now (the venue time stays).
md::OrderEvent delivered_at(md::OrderEvent e, std::uint64_t now) {
  std::visit([now](auto& v) { v.header.ts_init = UnixNanos{now}; }, e);
  return e;
}

struct Local {
  bool open = false;
  bool closed = false;
  std::uint64_t filled = 0;
};

Local local_of(Harness& h, std::size_t o) {
  const ex::OrderRecord& r = h.order(o);
  const md::OrderStatus s = r.state.status();
  return Local{ex::is_open(s) && s != md::OrderStatus::Submitted, ex::is_closed(s),
               r.state.filled().raw() / 1'000'000'000ULL};
}

// The spec's invariants, checked after every step.
void check_invariants(Harness& h, const Venue& venue) {
  const ex::SyncPhase phase = h.reconciler().phase();
  // HaltedUntilSynced (the node enters Running only once synced; see run_history).
  if (phase == ex::SyncPhase::Disconnected || phase == ex::SyncPhase::Buffering) {
    REQUIRE(h.trading().risk.trading_state() == md::TradingState::Halted);
  }
  // CountedOnce: every counted trade moved both positions by one unit, once.
  std::uint64_t filled = 0;
  for (std::size_t o = 0; o < venue.size(); ++o) {
    filled += local_of(h, o).filled;
    // NoPhantom
    if (phase == ex::SyncPhase::Synced) {
      REQUIRE(local_of(h, o).filled <= venue.order(o).fills);
    }
  }
  REQUIRE(h.strategy_position() == static_cast<std::int64_t>(filled * 1'000'000'000ULL));
  if (phase == ex::SyncPhase::Synced) {
    REQUIRE(h.venue_position() == static_cast<std::int64_t>(filled * 1'000'000'000ULL));
  }
}

void run_history(Gen& g, std::size_t orders, std::uint64_t max_fill, int steps) {
  st::KernelConfig c = config();
  c.trading.reconcile.lost_grace = jarvis::core::DurationNanos{~0ULL / 2}; // venue "none"
  const std::string qty = std::to_string(max_fill) + ".000";
  Harness h{orders, qty, c};
  h.run({perpetual_definition(1), running(2)});
  Venue venue{orders, max_fill};
  std::uint64_t now = 10;
  REQUIRE(h.step(stream(false, ++now)) == Status::Ok); // the spec's Init: Disconnected, halted
  std::optional<Snapshot> snap;
  bool requested = false;

  const auto reconcile = [&] {
    REQUIRE(h.step(snap->event(++now)) == Status::Ok);
    REQUIRE(h.reconciler().phase() == ex::SyncPhase::Synced);
    REQUIRE(h.step(running(++now)) == Status::Ok); // the driver's sync gate
    snap.reset();
    requested = false;
  };
  const auto deliver = [&](bool keep) {
    std::vector<Venue::Message>& chan = venue.chan();
    const auto i = static_cast<std::size_t>(g.below(chan.size()));
    const md::OrderEvent e = delivered_at(chan[i].event, ++now);
    if (!keep) {
      chan.erase(chan.begin() + static_cast<std::ptrdiff_t>(i));
    }
    REQUIRE(h.step(as_event(e)) == Status::Ok);
  };

  for (int step = 0; step < steps; ++step) {
    const ex::SyncPhase phase = h.reconciler().phase();
    switch (g.below(7)) {
    case 0: { // a venue change
      const auto o = static_cast<std::size_t>(g.below(orders));
      static_cast<void>(venue.change(o, g, h.ids[o]));
      break;
    }
    case 1:
      if (venue.up && g.chance(1, 4)) { // Disconnect
        venue.up = false;
        venue.chan().clear();
        snap.reset();
        requested = false;
        REQUIRE(h.step(stream(false, ++now)) == Status::Ok);
      } else if (!venue.up) { // Connect
        venue.up = true;
        REQUIRE(h.step(stream(true, ++now)) == Status::Ok);
      }
      break;
    case 2:
    case 3:
      if (venue.up && !venue.chan().empty()) {
        deliver(g.chance(1, 5));
      }
      break;
    case 4: // RequestSnapshot, SnapshotTaken
      if (phase == ex::SyncPhase::Buffering && !snap) {
        if (!requested) {
          requested = true;
        } else {
          snap = venue.snapshot(h.ids);
        }
      }
      break;
    default: // Reconcile
      if (phase == ex::SyncPhase::Buffering && snap) {
        reconcile();
      }
      break;
    }
    check_invariants(h, venue);
  }

  // Liveness: connect, sync, deliver everything; then the local state is the venue's.
  if (!venue.up) {
    venue.up = true;
    REQUIRE(h.step(stream(true, ++now)) == Status::Ok);
  }
  if (h.reconciler().phase() != ex::SyncPhase::Synced) {
    snap = venue.snapshot(h.ids);
    reconcile();
  }
  while (!venue.chan().empty()) {
    deliver(false);
    check_invariants(h, venue);
  }
  // Converged
  for (std::size_t o = 0; o < orders; ++o) {
    const Venue::Order& v = venue.order(o);
    const Local l = local_of(h, o);
    CAPTURE(o);
    REQUIRE(l.filled == v.fills);
    REQUIRE(l.open == (v.st == Venue::St::Open));
    REQUIRE(l.closed == (v.st == Venue::St::Done));
    // The exact status too: a late status older than the order's latest is stale.
    md::OrderStatus expected = md::OrderStatus::Submitted;
    if (v.st == Venue::St::Open) {
      expected = v.fills == 0 ? md::OrderStatus::Accepted : md::OrderStatus::PartiallyFilled;
    } else if (v.st == Venue::St::Done) {
      expected = v.fills == max_fill ? md::OrderStatus::Filled : md::OrderStatus::Canceled;
    }
    REQUIRE(h.order(o).state.status() == expected);
  }
  REQUIRE(h.venue_position() == h.strategy_position());
}

} // namespace

TEST_SUITE("property") {
  TEST_CASE("the countdown is renewed for instruments with open orders while running") {
    constexpr std::uint64_t kSecond = 1'000'000'000;
    const auto fire_next = [](Harness& h) {
      jarvis::core::FiredTimer t;
      REQUIRE(h.engine().next_timer(t));
      CHECK(t.key == jarvis::core::TimerKey{st::kKernelTimerOwner, st::kCountdownTimerId});
      h.outputs.clear();
      h.run({md::Event{md::TimerFired{t.key, t.deadline, t.deadline}}});
      return t.deadline.value();
    };
    const auto countdowns = [](const Harness& h) {
      std::vector<std::uint64_t> out;
      for (const md::CountdownCancelAll& c : h.outputs_of<md::CountdownCancelAll>()) {
        CHECK(c.instrument_id == iid(kBtc));
        CHECK(c.countdown_ms == 120'000);
        out.push_back(c.ts_init.value());
      }
      return out;
    };

    st::KernelConfig c = config();
    c.trading.risk.countdown_cancel_ms = 120'000;
    Harness h{1, "1.000", c};
    start(h, 1);
    // No open orders when Running began; the order the strategy submitted then is covered in
    // the same step, after its SubmitOrder.
    CHECK(countdowns(h) == std::vector<std::uint64_t>{2});
    REQUIRE(h.outputs.size() >= 2);
    CHECK(std::holds_alternative<md::SubmitOrder>(h.outputs[h.outputs.size() - 2]));

    // Renewed a quarter of the countdown apart while the order is open.
    CHECK(fire_next(h) == 2 + 30 * kSecond);
    CHECK(countdowns(h) == std::vector<std::uint64_t>{2 + 30 * kSecond});

    // Out of Running the renewals stop: the venue cancels if the node stays out.
    h.run({lifecycle(NodeState::Running, NodeState::Degraded, 40 * kSecond)});
    CHECK(fire_next(h) == 2 + 60 * kSecond);
    CHECK(countdowns(h).empty());
    jarvis::core::FiredTimer none;
    CHECK_FALSE(h.engine().next_timer(none));

    // Back in Running: renewed at once, then on the timer again.
    h.outputs.clear();
    h.run(
        {lifecycle(NodeState::Degraded, NodeState::Syncing, 69 * kSecond), running(70 * kSecond)});
    CHECK(countdowns(h) == std::vector<std::uint64_t>{70 * kSecond});

    // Without open orders the renewal sends nothing but keeps the timer.
    h.run({md::Event{venue_event<md::OrderCanceled>(80 * kSecond, 80 * kSecond, h.ids[0])}});
    CHECK(fire_next(h) == 100 * kSecond);
    CHECK(countdowns(h).empty());
    CHECK(h.engine().next_timer(none));
  }

  TEST_CASE("without a countdown the kernel sends none and arms no timer") {
    Harness h{1};
    start(h, 1);
    CHECK(h.outputs_of<md::CountdownCancelAll>().empty());
    jarvis::core::FiredTimer none;
    CHECK_FALSE(h.engine().next_timer(none));
  }

  TEST_CASE("random venue histories: halted until synced, counted once, no phantom, converged") {
    jarvis::testkit::for_all([](Gen& g) {
      const auto orders = static_cast<std::size_t>(g.range_u(1, 3));
      const std::uint64_t max_fill = g.range_u(1, 3);
      run_history(g, orders, max_fill, 150);
    });
  }
}
