#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/status.hpp"
#include "jarvis/execution/execution_engine.hpp"
#include "jarvis/execution/oms.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/generated/enums.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/order_events.hpp"
#include "jarvis/testkit/property.hpp"
#include "specs/map/order_lifecycle_actions.hpp"

namespace {

namespace ex = jarvis::execution;
namespace m = jarvis::model;
using jarvis::core::Status;
using m::OrderStatus;
using K = ex::OrderEventKind;

m::Quantity q(std::uint64_t units) {
  m::Quantity out;
  REQUIRE(m::Quantity::from_raw(units * 1'000'000'000ULL, 0, out) == Status::Ok);
  return out;
}

std::uint64_t units(m::Quantity v) { return v.raw() / 1'000'000'000ULL; }

m::Price px(std::string_view text) {
  m::Price out;
  REQUIRE(m::Price::parse(text, out) == Status::Ok);
  return out;
}

template <typename Id> Id make_id(const std::string& text) {
  Id out;
  REQUIRE(Id::from(text, out) == Status::Ok);
  return out;
}

m::ClientOrderId cid(int n) { return make_id<m::ClientOrderId>("C-" + std::to_string(n)); }
m::TradeId tid(std::string_view text) { return make_id<m::TradeId>(std::string{text}); }

ex::OrderRecord order_record(int n, m::OrderSide side = m::OrderSide::Buy,
                             std::uint64_t quantity = 2, std::uint32_t slot = 0) {
  ex::OrderRecord r;
  r.client_order_id = cid(n);
  r.side = side;
  r.slot = slot;
  r.price = px("100.00");
  r.state = ex::OrderState{q(quantity)};
  return r;
}

// Creates order n and brings it to ACCEPTED.
std::uint32_t working(ex::Oms& oms, int n, m::OrderSide side = m::OrderSide::Buy,
                      std::uint64_t quantity = 2, std::uint32_t slot = 0) {
  std::uint32_t index = ex::kNoIndex;
  REQUIRE(oms.create(order_record(n, side, quantity, slot), index) == Status::Ok);
  REQUIRE(oms.apply(index, K::Submitted) == Status::Ok);
  REQUIRE(oms.apply(index, K::Accepted) == Status::Ok);
  return index;
}

using Triple = std::tuple<std::string, std::string, std::string>;

// The Transitions set of specs/tla/OrderLifecycle.tla.
std::set<Triple> spec_transitions() {
  std::ifstream file(std::string{JARVIS_SOURCE_DIR} + "/specs/tla/OrderLifecycle.tla");
  REQUIRE(file.good());
  const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const std::size_t begin = text.find("\\* BEGIN TRANSITIONS");
  const std::size_t end = text.find("\\* END TRANSITIONS");
  REQUIRE(begin != std::string::npos);
  REQUIRE(end != std::string::npos);
  std::set<Triple> out;
  std::size_t pos = begin;
  while ((pos = text.find("<<", pos)) != std::string::npos && pos < end) {
    const std::size_t close = text.find(">>", pos);
    std::array<std::string, 3> parts;
    std::size_t cursor = pos;
    for (std::string& part : parts) {
      const std::size_t open = text.find('"', cursor);
      const std::size_t shut = text.find('"', open + 1);
      part = text.substr(open + 1, shut - open - 1);
      cursor = shut + 1;
    }
    out.emplace(parts[0], parts[1], parts[2]);
    pos = close;
  }
  return out;
}

// ---- exhaustive exploration of the C++ order state, as TLC explores the spec ----------------

constexpr std::uint64_t kMaxQty = 3;
constexpr std::size_t kTrades = 3;

struct Model {
  OrderStatus status = OrderStatus::Initialized;
  std::optional<OrderStatus> prev;
  std::uint64_t quantity = 1;
  std::uint64_t filled = 0;
  std::array<int, kTrades> fills{-1, -1, -1}; // -1: trade not seen

  auto key() const { return std::tuple{status, prev, quantity, filled, fills}; }
  friend bool operator<(const Model& a, const Model& b) { return a.key() < b.key(); }
};

ex::OrderState state_of(const Model& s) {
  ex::OrderState out;
  REQUIRE(ex::OrderState::restore(s.status, s.prev, q(s.quantity), q(s.filled), out) == Status::Ok);
  return out;
}

Model model_of(const ex::OrderState& st, std::array<int, kTrades> fills) {
  return Model{st.status(), st.previous(), units(st.quantity()), units(st.filled()), fills};
}

std::vector<Model> successors(const Model& s) {
  std::vector<Model> out;
  for (std::size_t k = 0; k < ex::kOrderEventKindCount; ++k) {
    ex::OrderState st = state_of(s);
    if (st.apply(static_cast<K>(k)) == Status::Ok) {
      out.push_back(model_of(st, s.fills));
    }
  }
  for (std::uint64_t n = 1; n <= kMaxQty; ++n) {
    ex::OrderState st = state_of(s);
    if (st.update(q(n)) == Status::Ok) {
      out.push_back(model_of(st, s.fills));
    }
  }
  for (std::size_t t = 0; t < kTrades; ++t) {
    for (std::uint64_t n = 1; n <= kMaxQty; ++n) {
      if (s.fills[t] < 0) { // the OMS refuses a second fill of one trade
        ex::OrderState st = state_of(s);
        if (st.fill(q(n)) == Status::Ok) {
          auto fills = s.fills;
          fills[t] = static_cast<int>(n);
          out.push_back(model_of(st, fills));
        }
      } else if (n <= static_cast<std::uint64_t>(s.fills[t])) { // void part of a known trade
        ex::OrderState st = state_of(s);
        if (st.void_fill(q(n)) == Status::Ok) {
          auto fills = s.fills;
          fills[t] -= static_cast<int>(n);
          out.push_back(model_of(st, fills));
        }
      }
    }
  }
  return out;
}

void check_invariants(const Model& s) {
  CHECK(s.filled <= s.quantity);
  std::uint64_t sum = 0;
  for (const int f : s.fills) {
    sum += f > 0 ? static_cast<std::uint64_t>(f) : 0;
  }
  CHECK(sum == s.filled);
  if (s.filled == s.quantity) {
    CHECK(s.status == OrderStatus::Filled);
  }
  if (ex::is_pending(s.status)) {
    CHECK(s.prev.has_value());
  }
}

} // namespace

TEST_SUITE("conformance") {
  TEST_CASE("the transition table equals the OrderLifecycle spec") {
    std::set<Triple> code;
    for (const ex::OrderTransition& t : ex::kOrderTransitions) {
      code.emplace(std::string{m::to_string(t.from)}, std::string{ex::to_string(t.kind)},
                   std::string{m::to_string(t.to)});
    }
    CHECK(code.size() == ex::kOrderTransitions.size()); // no duplicate edges
    const std::set<Triple> spec = spec_transitions();
    for (const Triple& t : spec) {
      INFO(std::get<0>(t) << " --" << std::get<1>(t) << "--> " << std::get<2>(t));
      CHECK(code.contains(t));
    }
    for (const Triple& t : code) {
      INFO(std::get<0>(t) << " --" << std::get<1>(t) << "--> " << std::get<2>(t));
      CHECK(spec.contains(t));
    }
  }

  TEST_CASE("the C++ order state reaches exactly the states TLC reaches") {
    std::set<Model> seen;
    std::deque<Model> frontier;
    for (std::uint64_t n = 1; n <= kMaxQty; ++n) {
      Model init;
      init.quantity = n;
      seen.insert(init);
      frontier.push_back(init);
    }
    while (!frontier.empty()) {
      const Model s = frontier.front();
      frontier.pop_front();
      check_invariants(s);
      for (const Model& next : successors(s)) {
        if (seen.insert(next).second) {
          frontier.push_back(next);
        }
      }
    }
    // TLC on specs/tla/OrderLifecycle.cfg (MaxQty = 3, three trade ids): 2085 distinct states.
    CHECK(seen.size() == 2085);
  }
}

TEST_SUITE("unit") {
  TEST_CASE("the jarvis edge: an IOC remainder can expire before the acceptance") {
    ex::OrderState st{q(2)};
    REQUIRE(st.apply(K::Submitted) == Status::Ok);
    CHECK(st.apply(K::Expired) == Status::Ok);
    CHECK(st.status() == OrderStatus::Expired);
  }

  TEST_CASE("a fill while a cancel is pending keeps it pending and remembers the fill") {
    ex::OrderState st{q(3)};
    REQUIRE(st.apply(K::Submitted) == Status::Ok);
    REQUIRE(st.apply(K::Accepted) == Status::Ok);
    REQUIRE(st.apply(K::PendingCancel) == Status::Ok);
    REQUIRE(st.fill(q(1)) == Status::Ok);
    CHECK(st.status() == OrderStatus::PendingCancel);
    CHECK(st.previous() == OrderStatus::PartiallyFilled);
    // The cancel is refused: back to where the order was.
    REQUIRE(st.apply(K::CancelRejected) == Status::Ok);
    CHECK(st.status() == OrderStatus::PartiallyFilled);
    CHECK(units(st.leaves()) == 2);
  }

  TEST_CASE("modify rejections and pending updates restore the previous status") {
    ex::OrderState st{q(3)};
    REQUIRE(st.apply(K::Submitted) == Status::Ok);
    REQUIRE(st.apply(K::Accepted) == Status::Ok);
    REQUIRE(st.apply(K::PendingUpdate) == Status::Ok);
    REQUIRE(st.apply(K::PendingUpdate) == Status::Ok); // a second request keeps ACCEPTED
    REQUIRE(st.apply(K::ModifyRejected) == Status::Ok);
    CHECK(st.status() == OrderStatus::Accepted);
    REQUIRE(st.apply(K::PendingUpdate) == Status::Ok);
    REQUIRE(st.update(q(5)) == Status::Ok);
    CHECK(st.status() == OrderStatus::Accepted);
    CHECK(units(st.quantity()) == 5);
  }

  TEST_CASE("fills complete the order, overtake a cancel, and never exceed the leaves") {
    ex::OrderState st{q(2)};
    REQUIRE(st.apply(K::Submitted) == Status::Ok);
    CHECK(st.fill(q(3)) == Status::InvalidArgument);
    REQUIRE(st.fill(q(2)) == Status::Ok); // SUBMITTED --FILLED--> FILLED: fill before the ack
    CHECK(st.status() == OrderStatus::Filled);
    CHECK(st.apply(K::Accepted) == Status::InvalidTransition); // the late ack is refused

    ex::OrderState late{q(2)};
    REQUIRE(late.apply(K::Submitted) == Status::Ok);
    REQUIRE(late.apply(K::Accepted) == Status::Ok);
    REQUIRE(late.apply(K::Canceled) == Status::Ok);
    REQUIRE(late.fill(q(1)) == Status::Ok); // a fill that raced the cancel
    CHECK(late.status() == OrderStatus::Canceled);
    CHECK(units(late.filled()) == 1);
  }

  TEST_CASE("a voided fill reopens a working order and voids a filled one") {
    ex::OrderState st{q(2)};
    REQUIRE(st.apply(K::Submitted) == Status::Ok);
    REQUIRE(st.apply(K::Accepted) == Status::Ok);
    REQUIRE(st.fill(q(1)) == Status::Ok);
    REQUIRE(st.void_fill(q(1)) == Status::Ok);
    CHECK(st.status() == OrderStatus::Accepted);
    REQUIRE(st.fill(q(2)) == Status::Ok);
    REQUIRE(st.void_fill(q(1)) == Status::Ok);
    CHECK(st.status() == OrderStatus::Voided);
    CHECK(st.void_fill(q(5)) == Status::InvalidArgument);
  }

  TEST_CASE("event kinds follow the order event variant") {
    ex::OrderEventKind k{};
    CHECK_FALSE(ex::kind_of(m::OrderEvent{m::OrderInitialized{}}, k));
    REQUIRE(ex::kind_of(m::OrderEvent{m::OrderFilled{}}, k));
    CHECK(k == K::Filled);
    REQUIRE(ex::kind_of(m::OrderEvent{m::OrderCancelRejected{}}, k));
    CHECK(k == K::CancelRejected);
    CHECK(jarvis::specmap::order_lifecycle::kinds_match());
  }
}

TEST_SUITE("unit") {
  TEST_CASE("the OMS finds orders by ClientOrderId and refuses a second order with the same id") {
    ex::Oms oms{8, 32};
    std::uint32_t a = ex::kNoIndex;
    std::uint32_t b = ex::kNoIndex;
    REQUIRE(oms.create(order_record(1), a) == Status::Ok);
    REQUIRE(oms.create(order_record(2), b) == Status::Ok);
    CHECK(oms.find(cid(1)) == a);
    CHECK(oms.find(cid(2)) == b);
    CHECK(oms.find(cid(3)) == ex::kNoIndex);
    std::uint32_t again = ex::kNoIndex;
    CHECK(oms.create(order_record(1), again) == Status::AlreadyExists);
    CHECK(oms.live() == 2);
    CHECK(oms.at(a).state.status() == OrderStatus::Initialized);
  }

  TEST_CASE("a trade fills an order once and the average price is truncated") {
    ex::Oms oms{8, 32};
    const std::uint32_t i = working(oms, 1);
    REQUIRE(oms.fill(i, tid("t1"), q(1), px("100.50")) == Status::Ok);
    CHECK(oms.fill(i, tid("t1"), q(1), px("100.50")) == Status::DuplicateFill);
    CHECK(oms.at(i).state.status() == OrderStatus::PartiallyFilled);
    REQUIRE(oms.fill(i, tid("t2"), q(1), px("101.05")) == Status::Ok);
    CHECK(oms.at(i).state.status() == OrderStatus::Filled);
    m::Price avg;
    REQUIRE(ex::Oms::average_price(oms.at(i), 3, avg));
    CHECK(avg == px("100.775"));
    REQUIRE(ex::Oms::average_price(oms.at(i), 2, avg));
    CHECK(avg == px("100.77")); // truncated toward zero, never rounded up
    CHECK(oms.fill(i, tid("t3"), q(1), px("100.00")) == Status::InvalidTransition);
  }

  TEST_CASE("voids name an earlier trade and never exceed it") {
    ex::Oms oms{8, 32};
    const std::uint32_t i = working(oms, 1, m::OrderSide::Sell, 3);
    REQUIRE(oms.fill(i, tid("t1"), q(2), px("100.00")) == Status::Ok);
    CHECK(oms.void_fill(i, tid("zz"), q(1), px("100.00")) == Status::InvalidArgument);
    CHECK(oms.void_fill(i, tid("t1"), q(3), px("100.00")) == Status::InvalidArgument);
    REQUIRE(oms.void_fill(i, tid("t1"), q(2), px("100.00")) == Status::Ok);
    CHECK(oms.at(i).state.status() == OrderStatus::Accepted);
    m::Price avg;
    CHECK_FALSE(ex::Oms::average_price(oms.at(i), 2, avg));
  }

  TEST_CASE("a full OMS evicts its oldest closed order, never an open one") {
    ex::Oms oms{2, 8};
    std::uint32_t a = ex::kNoIndex;
    std::uint32_t b = ex::kNoIndex;
    std::uint32_t c = ex::kNoIndex;
    REQUIRE(oms.create(order_record(1), a) == Status::Ok);
    REQUIRE(oms.create(order_record(2), b) == Status::Ok);
    CHECK(oms.create(order_record(3), c) == Status::CapacityExceeded);
    REQUIRE(oms.apply(b, K::Denied) == Status::Ok);
    REQUIRE(oms.apply(a, K::Denied) == Status::Ok);
    REQUIRE(oms.create(order_record(3), c) == Status::Ok);
    CHECK(c == b); // order 2 closed first, so it goes first
    CHECK(oms.find(cid(2)) == ex::kNoIndex);
    CHECK(oms.find(cid(1)) == a);
    CHECK(oms.find(cid(3)) == c);
    CHECK(oms.live() == 2);
  }

  TEST_CASE("evicted orders give their fill records back") {
    ex::Oms oms{1, 2};
    std::uint32_t i = working(oms, 1, m::OrderSide::Buy, 2);
    REQUIRE(oms.fill(i, tid("t1"), q(1), px("100.00")) == Status::Ok);
    REQUIRE(oms.fill(i, tid("t2"), q(1), px("100.00")) == Status::Ok);
    i = working(oms, 2, m::OrderSide::Buy, 2); // evicts order 1 and frees both records
    CHECK(oms.fill(i, tid("t1"), q(1), px("100.00")) == Status::Ok);
    CHECK(oms.fill(i, tid("t2"), q(1), px("100.00")) == Status::Ok);
  }

  TEST_CASE("open quantity sums the leaves of open orders per side") {
    ex::Oms oms{8, 32};
    const std::uint32_t a = working(oms, 1, m::OrderSide::Buy, 3);
    working(oms, 2, m::OrderSide::Sell, 2);
    const std::uint32_t c = working(oms, 3, m::OrderSide::Sell, 5, 1);
    REQUIRE(oms.fill(a, tid("t1"), q(1), px("100.00")) == Status::Ok);
    REQUIRE(oms.apply(c, K::Canceled) == Status::Ok);
    oms.at(a).strategy = 1;
    const ex::OpenQuantity all = oms.open_quantity(0);
    CHECK(all.buy_raw == q(2).raw());
    CHECK(all.sell_raw == q(2).raw());
    CHECK(all.orders == 2);
    CHECK(oms.open_quantity(0, std::uint16_t{1}).orders == 1);
    CHECK(oms.open_quantity(1).orders == 0);
  }

  TEST_CASE("venue events: acceptance records the venue id; the rest is told apart") {
    ex::Oms oms{8, 32};
    std::uint32_t i = ex::kNoIndex;
    REQUIRE(oms.create(order_record(1), i) == Status::Ok);
    REQUIRE(oms.apply(i, K::Submitted) == Status::Ok);

    m::OrderAccepted accepted;
    accepted.header.client_order_id = cid(1);
    accepted.venue_order_id = make_id<m::VenueOrderId>("88");
    std::uint32_t index = ex::kNoIndex;
    CHECK(ex::apply_order_event(oms, m::OrderEvent{accepted}, index) == ex::EventOutcome::Applied);
    CHECK(index == i);
    CHECK(oms.at(i).venue_order_id == make_id<m::VenueOrderId>("88"));

    CHECK(ex::apply_order_event(oms, m::OrderEvent{accepted}, index) ==
          ex::EventOutcome::Refused); // ACCEPTED --ACCEPTED--> is not a transition

    m::OrderFilled fill;
    fill.header.client_order_id = cid(1);
    fill.trade_id = tid("t1");
    fill.last_qty = q(1);
    fill.last_px = px("100.00");
    CHECK(ex::apply_order_event(oms, m::OrderEvent{fill}, index) == ex::EventOutcome::Applied);
    CHECK(ex::apply_order_event(oms, m::OrderEvent{fill}, index) ==
          ex::EventOutcome::DuplicateFill);
    fill.trade_id = tid("t2");
    fill.last_qty = q(5);
    CHECK(ex::apply_order_event(oms, m::OrderEvent{fill}, index) ==
          ex::EventOutcome::Refused); // an overfill

    m::OrderCanceled unknown;
    unknown.header.client_order_id = cid(9);
    CHECK(ex::apply_order_event(oms, m::OrderEvent{unknown}, index) ==
          ex::EventOutcome::UnknownOrder);
    m::OrderInitialized init;
    init.header.client_order_id = cid(1);
    CHECK(ex::apply_order_event(oms, m::OrderEvent{init}, index) == ex::EventOutcome::Refused);

    m::OrderUpdated updated;
    updated.header.client_order_id = cid(1);
    updated.quantity = q(4);
    updated.price = px("99.50");
    CHECK(ex::apply_order_event(oms, m::OrderEvent{updated}, index) == ex::EventOutcome::Applied);
    CHECK(units(oms.at(i).state.quantity()) == 4);
    CHECK(oms.at(i).price == px("99.50"));
  }

  TEST_CASE("venue events: a status older than the order's latest is stale; fills never are") {
    ex::Oms oms{8, 32};
    std::uint32_t i = ex::kNoIndex;
    REQUIRE(oms.create(order_record(1), i) == Status::Ok);
    REQUIRE(oms.apply(i, K::Submitted) == Status::Ok);
    std::uint32_t index = ex::kNoIndex;
    const auto at = [](auto e, std::uint64_t ts) {
      e.header.client_order_id = cid(1);
      e.header.ts_event = jarvis::core::UnixNanos{ts};
      return m::OrderEvent{e};
    };
    m::OrderAccepted accepted;
    accepted.venue_order_id = make_id<m::VenueOrderId>("88");
    REQUIRE(ex::apply_order_event(oms, at(accepted, 10), index) == ex::EventOutcome::Applied);
    m::OrderUpdated newer;
    newer.quantity = q(3);
    newer.price = px("99.00");
    REQUIRE(ex::apply_order_event(oms, at(newer, 20), index) == ex::EventOutcome::Applied);
    CHECK(oms.at(i).ts_venue == jarvis::core::UnixNanos{20});
    m::OrderUpdated older = newer;
    older.quantity = q(4);
    CHECK(ex::apply_order_event(oms, at(older, 15), index) == ex::EventOutcome::Stale);
    CHECK(units(oms.at(i).state.quantity()) == 3);
    CHECK(ex::apply_order_event(oms, at(older, 20), index) ==
          ex::EventOutcome::Applied); // the same venue time is not older
    m::OrderFilled fill;
    fill.trade_id = tid("t1");
    fill.last_qty = q(1);
    fill.last_px = px("99.00");
    CHECK(ex::apply_order_event(oms, at(fill, 5), index) == ex::EventOutcome::Applied);
    CHECK(oms.at(i).ts_venue == jarvis::core::UnixNanos{20});
  }
}

TEST_SUITE("property") {
  TEST_CASE("the OMS index agrees with a reference model through creates, closes and evictions") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      constexpr std::uint32_t kOrders = 16;
      ex::Oms oms{kOrders, 64};
      std::map<int, bool> present; // order -> closed?
      std::deque<int> closed;      // closing order, oldest first
      int next = 0;
      for (int step = 0; step < 400; ++step) {
        if (gen.chance(3, 5)) {
          std::uint32_t index = ex::kNoIndex;
          const Status s = oms.create(order_record(next), index);
          if (present.size() == kOrders && closed.empty()) {
            REQUIRE(s == Status::CapacityExceeded);
            continue;
          }
          REQUIRE(s == Status::Ok);
          if (present.size() == kOrders) {
            present.erase(closed.front());
            closed.pop_front();
          }
          present[next++] = false;
        } else if (!present.empty()) {
          auto it = present.begin();
          std::advance(it, static_cast<long>(gen.below(present.size())));
          if (!it->second) {
            REQUIRE(oms.apply(oms.find(cid(it->first)), K::Denied) == Status::Ok);
            it->second = true;
            closed.push_back(it->first);
          }
        }
        REQUIRE(oms.live() == present.size());
      }
      for (int n = 0; n < next; ++n) {
        const std::uint32_t index = oms.find(cid(n));
        CHECK((index != ex::kNoIndex) == present.contains(n));
        if (index != ex::kNoIndex) {
          CHECK(oms.at(index).client_order_id == cid(n));
        }
      }
    });
  }

  TEST_CASE("running open totals equal a scan of the open orders") {
    jarvis::testkit::for_all([](jarvis::testkit::Gen& gen) {
      ex::Oms oms{24, 128, 2};
      int next = 0;
      int trade = 0;
      const auto scan = [&](std::uint32_t slot) {
        ex::OpenQuantity total;
        for (std::uint16_t s = 0; s < 2; ++s) {
          const ex::OpenQuantity part = oms.open_quantity(slot, s);
          total.buy_raw += part.buy_raw;
          total.sell_raw += part.sell_raw;
          total.orders += part.orders;
          total.buy_notional += part.buy_notional;
          total.sell_notional += part.sell_notional;
        }
        return total;
      };
      for (int step = 0; step < 300; ++step) {
        const std::uint64_t action = gen.below(6);
        if (action == 0 || next == 0) {
          ex::OrderRecord r =
              order_record(next, gen.coin() ? m::OrderSide::Buy : m::OrderSide::Sell,
                           1 + gen.below(4), static_cast<std::uint32_t>(gen.below(2)));
          r.strategy = static_cast<std::uint16_t>(gen.below(2));
          std::uint32_t index = ex::kNoIndex;
          if (oms.create(r, index) == Status::Ok) {
            static_cast<void>(oms.apply(index, K::Submitted));
            ++next;
          }
        } else {
          const std::uint32_t index =
              oms.find(cid(static_cast<int>(gen.below(static_cast<std::uint64_t>(next)))));
          if (index == ex::kNoIndex) {
            continue;
          }
          switch (action) {
          case 1:
            static_cast<void>(oms.apply(index, K::Accepted));
            break;
          case 2:
            static_cast<void>(
                oms.fill(index, tid("t" + std::to_string(trade++)), q(1), px("100.00")));
            break;
          case 3:
            static_cast<void>(oms.apply(index, K::PendingUpdate));
            static_cast<void>(oms.update(index, q(2 + gen.below(4)), px("99.50")));
            break;
          case 4:
            static_cast<void>(oms.apply(index, K::Canceled));
            break;
          default:
            static_cast<void>(oms.apply(index, K::PendingCancel));
            break;
          }
        }
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
          const ex::OpenQuantity fast = oms.open_quantity(slot);
          const ex::OpenQuantity slow = scan(slot);
          REQUIRE(fast.buy_raw == slow.buy_raw);
          REQUIRE(fast.sell_raw == slow.sell_raw);
          REQUIRE(fast.orders == slow.orders);
          REQUIRE(fast.buy_notional == slow.buy_notional);
          REQUIRE(fast.sell_notional == slow.sell_notional);
        }
      }
    });
  }
}
