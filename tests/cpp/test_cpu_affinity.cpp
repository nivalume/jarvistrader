// Thread placement from [threads] (jarvis/live/cpu_affinity.hpp).

#include <doctest/doctest.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "jarvis/live/cpu_affinity.hpp"

#if defined(__linux__)
#include <sched.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace live = jarvis::live;
namespace node = jarvis::node;
using jarvis::core::Status;

namespace {

#if defined(__linux__)
int current_cpu() { return sched_getcpu(); }
#elif defined(_WIN32)
int current_cpu() { return static_cast<int>(::GetCurrentProcessorNumber()); }
#endif

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("cpu lists parse the kernel's format and fold back") {
    std::vector<int> cpus;
    REQUIRE(live::parse_cpu_list("0-3,8,10-11\n", cpus));
    CHECK(cpus == std::vector<int>{0, 1, 2, 3, 8, 10, 11});
    CHECK(live::cpu_list_text(cpus) == "0-3,8,10-11");
    REQUIRE(live::parse_cpu_list("5,1,1", cpus));
    CHECK(cpus == std::vector<int>{1, 5});
    REQUIRE(live::parse_cpu_list("", cpus));
    CHECK(cpus.empty());
    CHECK(live::cpu_list_text(cpus).empty());
    for (const char* bad : {"3-1", "a", "1,", "-1", "1-", "1024", "0-1024", "1 2"}) {
      CAPTURE(bad);
      CHECK_FALSE(live::parse_cpu_list(bad, cpus));
    }
  }

  TEST_CASE("an empty [threads] pins nothing") {
    live::ThreadPlacement p;
    std::string error;
    const std::vector<int> allowed{0, 1, 2, 3};
    node::ThreadsSection t;
    t.busy_poll = true;
    REQUIRE(live::place_threads(t, allowed, std::nullopt, p, error) == Status::Ok);
    CHECK(p.busy_poll);
    CHECK(p.core.empty());
    CHECK(p.market.empty());
    CHECK(p.venue.empty());
    CHECK(p.others.empty());
  }

  TEST_CASE("the pool is what the pinned threads leave") {
    live::ThreadPlacement p;
    std::string error;
    const std::vector<int> allowed{3, 2, 1, 0}; // any order
    node::ThreadsSection t;
    t.core_cpu = 2;
    REQUIRE(live::place_threads(t, allowed, std::nullopt, p, error) == Status::Ok);
    CHECK(p.core == std::vector<int>{2});
    CHECK(p.market == std::vector<int>{0, 1, 3});
    CHECK(p.venue == std::vector<int>{0, 1, 3});
    CHECK(p.others == std::vector<int>{0, 1, 3});

    // Busy-polling IO threads keep their CPUs to themselves; sleeping ones share theirs.
    t.market_cpu = 3;
    t.venue_cpu = 1;
    REQUIRE(live::place_threads(t, allowed, std::nullopt, p, error) == Status::Ok);
    CHECK(p.market == std::vector<int>{3});
    CHECK(p.venue == std::vector<int>{1});
    CHECK(p.others == std::vector<int>{0, 1, 3});
    t.busy_poll = true;
    REQUIRE(live::place_threads(t, allowed, std::nullopt, p, error) == Status::Ok);
    CHECK(p.others == std::vector<int>{0});

    // Nothing left for the rest.
    t.venue_cpu = 0;
    t.market_cpu = 1;
    t.core_cpu = 2;
    const std::vector<int> three{0, 1, 2};
    CHECK(live::place_threads(t, three, std::nullopt, p, error) == Status::InvalidArgument);
    CHECK(error.find("no CPU is left") != std::string::npos);
  }

  TEST_CASE("a NUMA node narrows the pool and must hold the pinned CPUs") {
    live::ThreadPlacement p;
    std::string error;
    const std::vector<int> allowed{0, 1, 2, 3, 4, 5, 6, 7};
    node::ThreadsSection t;
    t.numa_node = 1;
    const std::optional<std::vector<int>> node1{std::vector<int>{4, 5, 6, 7}};
    REQUIRE(live::place_threads(t, allowed, node1, p, error) == Status::Ok);
    CHECK(p.core == std::vector<int>{4, 5, 6, 7});
    CHECK(p.others == std::vector<int>{4, 5, 6, 7});
    t.core_cpu = 5;
    REQUIRE(live::place_threads(t, allowed, node1, p, error) == Status::Ok);
    CHECK(p.core == std::vector<int>{5});
    CHECK(p.market == std::vector<int>{4, 6, 7});
    t.market_cpu = 1;
    CHECK(live::place_threads(t, allowed, node1, p, error) == Status::InvalidArgument);
    CHECK(error == "threads.market_cpu: CPU 1 is not on NUMA node 1 (4-7)");
    t.market_cpu = 9;
    CHECK(live::place_threads(t, allowed, node1, p, error) == Status::InvalidArgument);
    CHECK(error == "threads.market_cpu: CPU 9 is not available to this process (0-7)");
  }

#if defined(__linux__) || defined(_WIN32)
  TEST_CASE("threads run where they are pinned, and the core thread gets its CPUs back") {
    std::vector<int> allowed;
    std::string error;
    REQUIRE(live::allowed_cpus(allowed, error) == Status::Ok);
    REQUIRE_FALSE(allowed.empty());
    const int last = allowed.back();

    // The worker reads its CPU only once pinned (the kernel has moved it by then).
    std::atomic<bool> pinned{false};
    int seen = -1;
    std::thread worker{[&seen, &pinned] {
      while (!pinned.load()) {
        std::this_thread::yield();
      }
      seen = current_cpu();
    }};
    const std::vector<int> one{last};
    const Status s = live::pin_thread(worker, one, error);
    pinned.store(true);
    worker.join();
    REQUIRE(s == Status::Ok);
    CHECK(seen == last);

    {
      live::ScopedPin pin;
      REQUIRE(pin.pin(one, error) == Status::Ok);
#if defined(__linux__) // Windows reads the process's CPUs, not the thread's
      std::vector<int> now;
      REQUIRE(live::allowed_cpus(now, error) == Status::Ok);
      CHECK(now == one);
#endif
      CHECK(current_cpu() == last);
    }
    std::vector<int> after;
    REQUIRE(live::allowed_cpus(after, error) == Status::Ok);
    CHECK(after == allowed);

    const std::vector<int> outside{1023};
    std::thread idle{[] {}};
    CHECK(live::pin_thread(idle, outside, error) != Status::Ok);
    idle.join();

    std::vector<int> node0;
    if (live::numa_node_cpus(0, node0, error) == Status::Ok) {
      CHECK_FALSE(node0.empty());
    }
    CHECK(live::numa_node_cpus(1023, node0, error) == Status::InvalidArgument);
    CHECK(error.find("no NUMA node 1023") != std::string::npos);
  }
#endif
}
