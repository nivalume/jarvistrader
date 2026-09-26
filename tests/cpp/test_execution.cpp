#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/status.hpp"
#include "jarvis/execution/order.hpp"
#include "jarvis/execution/order_fsm.hpp"
#include "jarvis/model/generated/enums.hpp"
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
