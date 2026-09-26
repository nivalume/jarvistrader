#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/engine/lifecycle.hpp"
#include "jarvis/model/event.hpp"
#include "jarvis/testkit/property.hpp"

namespace {

namespace e = jarvis::engine;
using jarvis::core::Status;
using jarvis::core::UnixNanos;
using jarvis::model::LifecycleReason;
using jarvis::model::NodeState;
using jarvis::testkit::Gen;

constexpr std::array<NodeState, e::kNodeStateCount> kStates = {
    NodeState::Init,     NodeState::Wired,   NodeState::Starting,
    NodeState::Syncing,  NodeState::Running, NodeState::Degraded,
    NodeState::Stopping, NodeState::Stopped, NodeState::Faulted};
constexpr std::array<LifecycleReason, e::kLifecycleReasonCount> kReasons = {
    LifecycleReason::Configured, LifecycleReason::RunRequested,
    LifecycleReason::Started,    LifecycleReason::Synced,
    LifecycleReason::HealthLost, LifecycleReason::HealthRestored,
    LifecycleReason::EndOfData,  LifecycleReason::ShutdownRequested,
    LifecycleReason::Drained,    LifecycleReason::Fault};

NodeState run(std::initializer_list<LifecycleReason> reasons) {
  e::Lifecycle machine;
  jarvis::model::NodeLifecycle event;
  std::uint64_t ts = 1;
  for (const LifecycleReason r : reasons) {
    REQUIRE(machine.apply(r, UnixNanos{ts++}, event) == Status::Ok);
  }
  return machine.state();
}

// States reachable from `start` through the transition table.
std::set<NodeState> reachable(NodeState start) {
  std::set<NodeState> seen{start};
  std::vector<NodeState> frontier{start};
  while (!frontier.empty()) {
    const NodeState s = frontier.back();
    frontier.pop_back();
    for (const e::LifecycleTransition& t : e::kLifecycleTransitions) {
      if (t.from == s && seen.insert(t.to).second) {
        frontier.push_back(t.to);
      }
    }
  }
  return seen;
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the backtest path runs Init to Stopped") {
    CHECK(run({LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started,
               LifecycleReason::Synced, LifecycleReason::EndOfData, LifecycleReason::Drained}) ==
          NodeState::Stopped);
  }

  TEST_CASE("a live node degrades, resyncs and shuts down") {
    CHECK(
        run({LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started,
             LifecycleReason::Synced, LifecycleReason::HealthLost, LifecycleReason::HealthRestored,
             LifecycleReason::Synced, LifecycleReason::ShutdownRequested,
             LifecycleReason::ShutdownRequested, LifecycleReason::Drained}) == NodeState::Stopped);
  }

  TEST_CASE("each transition records a NodeLifecycle event") {
    e::Lifecycle machine;
    jarvis::model::NodeLifecycle event;
    REQUIRE(machine.apply(LifecycleReason::Configured, UnixNanos{5}, event) == Status::Ok);
    CHECK(event.from == NodeState::Init);
    CHECK(event.to == NodeState::Wired);
    CHECK(event.reason == LifecycleReason::Configured);
    CHECK(event.ts_init == UnixNanos{5});
    const jarvis::model::NodeLifecycle before = event;
    CHECK(machine.apply(LifecycleReason::Synced, UnixNanos{6}, event) == Status::InvalidTransition);
    CHECK(machine.state() == NodeState::Wired);
    CHECK(event.ts_init == before.ts_init); // untouched on failure
  }

  TEST_CASE("the transition table is a function and only terminal states are dead ends") {
    std::set<std::pair<NodeState, LifecycleReason>> keys;
    for (const e::LifecycleTransition& t : e::kLifecycleTransitions) {
      CHECK(keys.insert({t.from, t.reason}).second);
      CHECK_FALSE(e::is_terminal(t.from));
    }
    for (const NodeState s : kStates) {
      bool has_exit = false;
      for (const LifecycleReason r : kReasons) {
        NodeState to = s;
        const Status status = e::next_state(s, r, to);
        CHECK((status == Status::Ok) == keys.contains({s, r}));
        has_exit = has_exit || status == Status::Ok;
      }
      CHECK(has_exit != e::is_terminal(s));
    }
  }

  TEST_CASE("every state is reachable and every live state can reach both terminals") {
    CHECK(reachable(NodeState::Init).size() == e::kNodeStateCount);
    for (const NodeState s : kStates) {
      if (e::is_terminal(s)) {
        CHECK(reachable(s).size() == 1);
        continue;
      }
      const std::set<NodeState> r = reachable(s);
      CHECK(r.contains(NodeState::Faulted));
      if (s != NodeState::Init) {
        CHECK(r.contains(NodeState::Stopped));
      }
    }
  }

  TEST_CASE("Running is entered only from Syncing") {
    for (const e::LifecycleTransition& t : e::kLifecycleTransitions) {
      if (t.to == NodeState::Running) {
        CHECK(t.from == NodeState::Syncing);
        CHECK(t.reason == LifecycleReason::Synced);
      }
    }
    CHECK(e::strategies_active(NodeState::Running));
    CHECK_FALSE(e::strategies_active(NodeState::Syncing));
    CHECK_FALSE(e::strategies_active(NodeState::Degraded));
  }

  TEST_CASE("replay accepts recorded transitions and rejects tampered ones") {
    e::Lifecycle live;
    std::vector<jarvis::model::NodeLifecycle> log;
    for (const LifecycleReason r :
         {LifecycleReason::Configured, LifecycleReason::RunRequested, LifecycleReason::Started}) {
      jarvis::model::NodeLifecycle event;
      REQUIRE(live.apply(r, UnixNanos{1}, event) == Status::Ok);
      log.push_back(event);
    }
    e::Lifecycle replayed;
    for (const auto& event : log) {
      REQUIRE(replayed.replay(event) == Status::Ok);
    }
    CHECK(replayed.state() == live.state());

    e::Lifecycle tampered;
    jarvis::model::NodeLifecycle forged = log[0];
    forged.to = NodeState::Running;
    CHECK(tampered.replay(forged) == Status::InvalidTransition);
    CHECK(tampered.state() == NodeState::Init);
  }
}

TEST_SUITE("property") {
  TEST_CASE("random reason sequences follow the table and never leave a terminal state") {
    jarvis::testkit::for_all([](Gen& gen) {
      e::Lifecycle machine;
      jarvis::model::NodeLifecycle event;
      for (int step = 0; step < 64; ++step) {
        const LifecycleReason r = gen.pick(std::span<const LifecycleReason>{kReasons});
        const NodeState before = machine.state();
        NodeState expected = before;
        const Status table = e::next_state(before, r, expected);
        const Status applied = machine.apply(r, UnixNanos{static_cast<std::uint64_t>(step)}, event);
        CHECK(applied == table);
        CHECK(machine.state() == (jarvis::core::ok(applied) ? expected : before));
        if (e::is_terminal(before)) {
          CHECK(machine.state() == before);
        }
      }
    });
  }
}
