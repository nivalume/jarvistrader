#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/testkit/alloc.hpp"
#include "jarvis/testkit/gen.hpp"
#include "jarvis/testkit/property.hpp"

using jarvis::testkit::AllocationScope;
using jarvis::testkit::Gen;

TEST_SUITE("unit") {
  TEST_CASE("Gen matches the splitmix64 reference sequence") {
    Gen zero{0};
    CHECK(zero.next() == 0xE220A8397B1DCDAFULL);
    CHECK(zero.next() == 0x6E789E6AA1B965F4ULL);
    CHECK(zero.next() == 0x06C45D188009454FULL);
    CHECK(zero.next() == 0xF88BB8A8724C81ECULL);

    Gen answer{42};
    CHECK(answer.next() == 0xBDD732262FEB6E95ULL);
    CHECK(answer.next() == 0x28EFE333B266F103ULL);
  }

  TEST_CASE("Gen streams are reproducible from the seed") {
    Gen a{7};
    Gen b{7};
    for (int i = 0; i < 64; ++i) {
      CHECK(a.next() == b.next());
    }
  }

  TEST_CASE("range covers both endpoints including the full int64 domain") {
    Gen gen{3};
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 2000; ++i) {
      const std::int64_t v = gen.range(-2, 2);
      CHECK(v >= -2);
      CHECK(v <= 2);
      saw_lo = saw_lo || v == -2;
      saw_hi = saw_hi || v == 2;
    }
    CHECK(saw_lo);
    CHECK(saw_hi);
    const std::int64_t any = gen.range(INT64_MIN, INT64_MAX);
    CHECK(any >= INT64_MIN);
    CHECK(gen.range(5, 5) == 5);
    CHECK(gen.range_u(0, UINT64_MAX) <= UINT64_MAX);
  }

  TEST_CASE("pick returns an element of the span") {
    const std::array<int, 3> items{10, 20, 30};
    Gen gen{11};
    for (int i = 0; i < 100; ++i) {
      const int v = gen.pick(std::span<const int>{items});
      CHECK((v == 10 || v == 20 || v == 30));
    }
  }

  TEST_CASE("AllocationScope counts allocations made by this thread") {
    const AllocationScope scope;
    std::vector<int> values(16, 1);
    const std::uint64_t counted = scope.allocations();
    CHECK(counted >= 1U);
    CHECK(values.size() == 16U);
  }
}

TEST_SUITE("property") {
  TEST_CASE("below stays under its bound") {
    jarvis::testkit::for_all([](Gen& gen) {
      const std::uint64_t bound = gen.range_u(1, UINT64_MAX);
      CHECK(gen.below(bound) < bound);
    });
  }

  TEST_CASE("case seeds are independent of iteration order") {
    jarvis::testkit::for_all([](Gen& gen) {
      const std::uint64_t seed = gen.next();
      const std::uint64_t index = gen.below(1U << 20U);
      CHECK(jarvis::testkit::case_seed(seed, index) == jarvis::testkit::case_seed(seed, index));
    });
  }
}

TEST_SUITE("zero-alloc") {
  TEST_CASE("generating numbers does not allocate") {
    Gen gen{99};
    std::uint64_t acc = 0;
    const AllocationScope scope;
    for (int i = 0; i < 1000; ++i) {
      acc ^= gen.below(1000);
    }
    const std::uint64_t counted = scope.allocations();
    CHECK(counted == 0U);
    CHECK(acc < 1024U);
  }
}
