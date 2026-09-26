#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/core/arena.hpp"
#include "jarvis/core/clock.hpp"
#include "jarvis/core/crc32c.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/fixed_vector.hpp"
#include "jarvis/core/int_math.hpp"
#include "jarvis/core/priority_queue.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/core/sha256.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/testkit/alloc.hpp"
#include "jarvis/testkit/property.hpp"

namespace core = jarvis::core;
using core::Status;
using core::UnixNanos;
using jarvis::testkit::AllocationScope;
using jarvis::testkit::Gen;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

std::string hex(const core::Sha256::Digest& digest) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t b : digest) {
    out.push_back(kDigits[b >> 4U]);
    out.push_back(kDigits[b & 0x0FU]);
  }
  return out;
}

std::string rfc3339(std::uint64_t ns) {
  std::array<char, core::kRfc3339MaxLength> buffer{};
  std::size_t written = 0;
  REQUIRE(core::format_rfc3339(UnixNanos{ns}, buffer, written) == Status::Ok);
  return std::string{buffer.data(), written};
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("Status names are stable") {
    CHECK(core::to_string(Status::Ok) == "Ok");
    CHECK(core::to_string(Status::CapacityExceeded) == "CapacityExceeded");
    CHECK(core::ok(Status::Ok));
    CHECK_FALSE(core::ok(Status::Overflow));
  }

  TEST_CASE("FixedString enforces capacity and compares by content") {
    core::FixedString<4> a;
    CHECK(core::FixedString<4>::from("abcd", a) == Status::Ok);
    CHECK(a.view() == "abcd");
    core::FixedString<4> b;
    CHECK(core::FixedString<4>::from("abcde", b) == Status::OutOfRange);
    CHECK(b.empty());
    CHECK(core::FixedString<4>::from("abc", b) == Status::Ok);
    CHECK(b < a);
    CHECK(b != a);
  }

  TEST_CASE("FixedVector refuses to grow past its capacity") {
    core::FixedVector<int> v{2};
    CHECK(v.push_back(1) == Status::Ok);
    CHECK(v.push_back(2) == Status::Ok);
    CHECK(v.push_back(3) == Status::CapacityExceeded);
    CHECK(v.size() == 2);
    CHECK(v.swap_remove(0) == Status::Ok);
    CHECK(v[0] == 2);
    CHECK(v.swap_remove(5) == Status::OutOfRange);
  }

  TEST_CASE("Arena handles detect reuse of a slot") {
    core::Arena<int> arena{2};
    core::Arena<int>::HandleType h1;
    core::Arena<int>::HandleType h2;
    core::Arena<int>::HandleType h3;
    CHECK(arena.insert(10, h1) == Status::Ok);
    CHECK(arena.insert(20, h2) == Status::Ok);
    CHECK(arena.insert(30, h3) == Status::CapacityExceeded);
    CHECK(*arena.get(h1) == 10);
    CHECK(arena.erase(h1) == Status::Ok);
    CHECK(arena.get(h1) == nullptr);
    CHECK(arena.erase(h1) == Status::NotFound);
    CHECK(arena.insert(30, h3) == Status::Ok);
    CHECK(h3.index == h1.index);
    CHECK(h3.generation == h1.generation + 1U);
    CHECK(arena.get(h1) == nullptr);
    CHECK(*arena.get(h3) == 30);
    std::vector<int> seen;
    arena.for_each([&seen](core::Arena<int>::HandleType, int v) { seen.push_back(v); });
    CHECK(seen == std::vector<int>{30, 20});
  }

  TEST_CASE("mul_div computes exactly through 192 bits") {
    std::uint64_t out = 0;
    // (2^64-1)^2 * 3 / (2^64-1) / 3 == 2^64-1: the intermediate needs more than 128 bits.
    CHECK(core::mul_div_u64(UINT64_MAX, UINT64_MAX, 3, UINT64_MAX, out) == Status::Overflow);
    CHECK(core::mul_div_u64(UINT64_MAX, UINT64_MAX, 1, UINT64_MAX, out) == Status::Ok);
    CHECK(out == UINT64_MAX);
    CHECK(core::mul_div_u64(18'446'744'073'000'000'000ULL, 1'000'000'000ULL,
                            9'223'372'036'000'000'000ULL, 1'000'000'000'000'000'000ULL,
                            out) == Status::Overflow);
    CHECK(core::mul_div_u64(7, 1, 1, 0, out) == Status::InvalidArgument);
    std::int64_t signed_out = 0;
    CHECK(core::mul_div_i64(-7, 1, 1, 2, signed_out) == Status::Ok);
    CHECK(signed_out == -3); // toward zero
    CHECK(core::mul_div_i64(7, -1, -1, 2, signed_out) == Status::Ok);
    CHECK(signed_out == 3);
    CHECK(core::mul_div_i64(INT64_MIN, 1, 1, 1, signed_out) == Status::Ok);
    CHECK(signed_out == INT64_MIN);
    CHECK(core::mul_div_i64(INT64_MIN, -1, 1, 1, signed_out) == Status::Overflow);
  }

  TEST_CASE("CRC-32C check value") {
    CHECK(core::crc32c(bytes("123456789")) == 0xE3069283U);
    const std::uint32_t split = core::crc32c_finish(
        core::crc32c_extend(core::crc32c_extend(core::crc32c_init(), bytes("1234")), bytes("56789")));
    CHECK(split == 0xE3069283U);
  }

  TEST_CASE("SHA-256 test vectors") {
    core::Sha256 empty;
    CHECK(hex(empty.finish()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    core::Sha256 abc;
    abc.update(bytes("abc"));
    CHECK(hex(abc.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    core::Sha256 two_blocks;
    two_blocks.update(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
    CHECK(hex(two_blocks.finish()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  }

  TEST_CASE("Philox4x32-10 matches the Random123 known-answer vectors") {
    CHECK(core::philox4x32_10({0, 0, 0, 0}, {0, 0}) ==
          core::PhiloxCounter{0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8});
    CHECK(core::philox4x32_10({0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                              {0xffffffff, 0xffffffff}) ==
          core::PhiloxCounter{0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd});
    CHECK(core::philox4x32_10({0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344},
                              {0xa4093822, 0x299f31d0}) ==
          core::PhiloxCounter{0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1});
  }

  TEST_CASE("CounterRng draws depend only on their key") {
    const core::CounterRng a{42};
    const core::CounterRng b{42};
    CHECK(a.draw(7, 1) == b.draw(7, 1));
    CHECK(a.draw(7, 1) != a.draw(7, 2));
    CHECK(a.draw(7, 1) != a.draw(8, 1));
    CHECK(a.draw(7, 1, 0) != a.draw(7, 1, 1));
    CHECK(core::CounterRng{43}.draw(7, 1) != a.draw(7, 1));
    CHECK(core::mix64(0) == 0xE220A8397B1DCDAFULL);
  }

  TEST_CASE("RFC 3339 formatting matches nautilus") {
    CHECK(rfc3339(0) == "1970-01-01T00:00:00+00:00");
    CHECK(rfc3339(1'000'000'000) == "1970-01-01T00:00:01+00:00");
    CHECK(rfc3339(1'000'000'000'000'000'000ULL) == "2001-09-09T01:46:40+00:00");
    CHECK(rfc3339(1'500'000'000'000'000'000ULL) == "2017-07-14T02:40:00+00:00");
    CHECK(rfc3339(1'500'000'000'500'000'000ULL) == "2017-07-14T02:40:00.500+00:00");
    CHECK(rfc3339(1'500'000'000'123'456'000ULL) == "2017-07-14T02:40:00.123456+00:00");
    CHECK(rfc3339(1'500'000'000'123'456'789ULL) == "2017-07-14T02:40:00.123456789+00:00");
    CHECK(rfc3339(1'707'577'123'456'789'000ULL) == "2024-02-10T14:58:43.456789+00:00");
  }

  TEST_CASE("RFC 3339 parsing accepts offsets and rejects invalid instants") {
    UnixNanos t;
    CHECK(core::parse_rfc3339("2024-02-10T14:58:43.456789Z", t) == Status::Ok);
    CHECK(t.value() == 1'707'577'123'456'789'000ULL);
    CHECK(core::parse_rfc3339("2024-02-10T15:58:43.456789+01:00", t) == Status::Ok);
    CHECK(t.value() == 1'707'577'123'456'789'000ULL);
    CHECK(core::parse_rfc3339("2024-02-30T00:00:00Z", t) == Status::OutOfRange);
    CHECK(core::parse_rfc3339("1969-12-31T23:59:59Z", t) == Status::OutOfRange);
    CHECK(core::parse_rfc3339("2024-02-10T14:58:43.1234567891Z", t) == Status::PrecisionLoss);
    CHECK(core::parse_rfc3339("2024-02-10 14:58:43", t) == Status::ParseError);
    CHECK(core::parse_rfc3339("2024-02-10T14:58:43Zjunk", t) == Status::ParseError);
  }

  TEST_CASE("UnixNanos arithmetic is checked") {
    UnixNanos out;
    CHECK(UnixNanos{UINT64_MAX}.plus(core::DurationNanos{1}, out) == Status::Overflow);
    core::DurationNanos d;
    CHECK(UnixNanos{5}.since(UnixNanos{7}, d) == Status::OutOfRange);
    CHECK(UnixNanos{7}.since(UnixNanos{5}, d) == Status::Ok);
    CHECK(d.value() == 2);
  }

  TEST_CASE("EventKey orders by ts, then source, then seq") {
    const core::EventKey a{UnixNanos{1}, 2, 9};
    const core::EventKey b{UnixNanos{1}, 3, 0};
    const core::EventKey c{UnixNanos{2}, 0, 0};
    CHECK(a < b);
    CHECK(b < c);
    CHECK(core::EventKey{UnixNanos{1}, 2, 8} < a);
  }

  TEST_CASE("ReplayClock never moves backwards") {
    core::ReplayClock clock{UnixNanos{10}};
    CHECK(clock.advance_to(UnixNanos{20}) == Status::Ok);
    CHECK(clock.advance_to(UnixNanos{15}) == Status::InvalidArgument);
    CHECK(clock.now() == UnixNanos{20});
  }

  TEST_CASE("TimerQueue fires in deadline then schedule order") {
    core::TimerQueue timers{8};
    core::TimerHandle h;
    CHECK(timers.schedule(UnixNanos{100}, core::DurationNanos{}, {1, 1}, h) == Status::Ok);
    CHECK(timers.schedule(UnixNanos{50}, core::DurationNanos{}, {1, 2}, h) == Status::Ok);
    CHECK(timers.schedule(UnixNanos{100}, core::DurationNanos{}, {1, 3}, h) == Status::Ok);
    core::FiredTimer fired;
    CHECK_FALSE(timers.pop_due(UnixNanos{49}, fired));
    CHECK(timers.pop_due(UnixNanos{200}, fired));
    CHECK(fired.key.id == 2);
    CHECK(timers.pop_due(UnixNanos{200}, fired));
    CHECK(fired.key.id == 1);
    CHECK(timers.pop_due(UnixNanos{200}, fired));
    CHECK(fired.key.id == 3);
    CHECK_FALSE(timers.pop_due(UnixNanos{200}, fired));
    CHECK(timers.active() == 0);
  }

  TEST_CASE("TimerQueue re-arms periodic timers and honors cancel") {
    core::TimerQueue timers{4};
    core::TimerHandle periodic;
    core::TimerHandle once;
    CHECK(timers.schedule(UnixNanos{10}, core::DurationNanos{10}, {0, 7}, periodic) == Status::Ok);
    CHECK(timers.schedule(UnixNanos{15}, core::DurationNanos{}, {0, 8}, once) == Status::Ok);
    CHECK(timers.cancel(once) == Status::Ok);
    core::FiredTimer fired;
    std::vector<std::uint64_t> deadlines;
    while (timers.pop_due(UnixNanos{35}, fired)) {
      CHECK(fired.key.id == 7);
      deadlines.push_back(fired.deadline.value());
    }
    CHECK(deadlines == std::vector<std::uint64_t>{10, 20, 30});
    CHECK(timers.next_deadline() == UnixNanos{40});
    CHECK(timers.cancel(periodic) == Status::Ok);
    CHECK_FALSE(timers.next_deadline().has_value());
  }
}

TEST_SUITE("property") {
  TEST_CASE("mul_div agrees with 128-bit arithmetic where that suffices") {
    jarvis::testkit::for_all([](Gen& gen) {
      const std::uint64_t a = gen.range_u(0, UINT32_MAX);
      const std::uint64_t b = gen.range_u(0, UINT32_MAX);
      const std::uint64_t c = gen.range_u(0, UINT32_MAX);
      const std::uint64_t d = gen.range_u(1, UINT64_MAX);
      const core::u128 expected = static_cast<core::u128>(a) * b * c / d;
      std::uint64_t out = 0;
      const Status s = core::mul_div_u64(a, b, c, d, out);
      if (expected > UINT64_MAX) {
        CHECK(s == Status::Overflow);
      } else {
        CHECK(s == Status::Ok);
        CHECK(out == static_cast<std::uint64_t>(expected));
      }
    });
  }

  TEST_CASE("mul_div by a divisor that cancels a factor is exact") {
    jarvis::testkit::for_all([](Gen& gen) {
      const std::uint64_t a = gen.next();
      const std::uint64_t b = gen.range_u(1, UINT64_MAX);
      std::uint64_t out = 0;
      CHECK(core::mul_div_u64(a, b, 1, b, out) == Status::Ok);
      CHECK(out == a);
    });
  }

  TEST_CASE("RFC 3339 round-trips every instant") {
    jarvis::testkit::for_all([](Gen& gen) {
      // Up to year 2262 keeps the four-digit year and the full u64 range comfortably.
      const std::uint64_t ns = gen.range_u(0, 9'000'000'000'000'000'000ULL);
      UnixNanos parsed;
      CHECK(core::parse_rfc3339(rfc3339(ns), parsed) == Status::Ok);
      CHECK(parsed.value() == ns);
    });
  }

  TEST_CASE("civil date conversion round-trips") {
    jarvis::testkit::for_all([](Gen& gen) {
      const std::int64_t days = gen.range(-1'000'000, 1'000'000);
      const core::CivilDate date = core::civil_from_days(days);
      CHECK(core::days_from_civil(date.year, date.month, date.day) == days);
    });
  }

  TEST_CASE("PriorityQueue pops keys in strictly increasing order") {
    jarvis::testkit::for_all([](Gen& gen) {
      core::PriorityQueue<std::uint32_t> queue{64};
      const auto count = static_cast<std::uint32_t>(gen.range_u(0, 64));
      for (std::uint32_t i = 0; i < count; ++i) {
        const core::EventKey key{UnixNanos{gen.range_u(0, 5)},
                                 static_cast<std::uint16_t>(gen.range_u(0, 3)), i};
        REQUIRE(queue.push(key, i) == Status::Ok);
      }
      core::PriorityQueue<std::uint32_t>::Entry previous{};
      core::PriorityQueue<std::uint32_t>::Entry entry{};
      std::uint32_t popped = 0;
      while (queue.pop(entry)) {
        if (popped > 0) {
          CHECK(previous.key < entry.key);
        }
        previous = entry;
        ++popped;
      }
      CHECK(popped == count);
    });
  }

  TEST_CASE("TimerQueue keeps firing correctly under heavy cancellation") {
    jarvis::testkit::for_all([](Gen& gen) {
      constexpr std::uint32_t kCapacity = 8;
      core::TimerQueue timers{kCapacity};
      std::vector<core::TimerHandle> live;
      std::uint64_t expected_fires = 0;
      for (int step = 0; step < 200; ++step) {
        if (live.size() < kCapacity && gen.chance(2, 3)) {
          core::TimerHandle h;
          REQUIRE(timers.schedule(UnixNanos{gen.range_u(0, 1000)}, core::DurationNanos{},
                                  {0, static_cast<std::uint32_t>(step)}, h) == Status::Ok);
          live.push_back(h);
        } else if (!live.empty()) {
          const auto index = static_cast<std::size_t>(gen.below(live.size()));
          REQUIRE(timers.cancel(live[index]) == Status::Ok);
          live[index] = live.back();
          live.pop_back();
        }
      }
      expected_fires = live.size();
      core::FiredTimer fired;
      std::uint64_t fires = 0;
      std::uint64_t last = 0;
      while (timers.pop_due(UnixNanos{1000}, fired)) {
        CHECK(fired.deadline.value() >= last);
        last = fired.deadline.value();
        ++fires;
      }
      CHECK(fires == expected_fires);
    });
  }
}

TEST_SUITE("zero-alloc") {
  TEST_CASE("queue, timer, arena and formatting operations do not allocate") {
    core::PriorityQueue<std::uint64_t> queue{256};
    core::TimerQueue timers{64};
    core::Arena<std::uint64_t> arena{64};
    std::array<char, core::kRfc3339MaxLength> buffer{};
    std::uint64_t sink = 0;

    const AllocationScope scope;
    for (std::uint64_t i = 0; i < 256; ++i) {
      static_cast<void>(queue.push(core::EventKey{UnixNanos{i % 7}, 0, i}, i));
    }
    core::PriorityQueue<std::uint64_t>::Entry entry{};
    while (queue.pop(entry)) {
      sink += entry.payload;
    }
    for (std::uint32_t i = 0; i < 64; ++i) {
      core::TimerHandle h;
      static_cast<void>(timers.schedule(UnixNanos{i}, core::DurationNanos{3}, {0, i}, h));
      core::Arena<std::uint64_t>::HandleType ah;
      static_cast<void>(arena.insert(i, ah));
      static_cast<void>(arena.erase(ah));
    }
    core::FiredTimer fired;
    for (int i = 0; i < 500 && timers.pop_due(UnixNanos{100}, fired); ++i) {
      sink += fired.deadline.value();
    }
    std::size_t written = 0;
    static_cast<void>(core::format_rfc3339(UnixNanos{sink}, buffer, written));
    sink += written;
    const std::uint64_t counted = scope.allocations();
    CHECK(counted == 0U);
    CHECK(sink > 0U);
  }
}
