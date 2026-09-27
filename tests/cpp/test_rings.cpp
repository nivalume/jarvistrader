// The SPSC rings between the live threads (docs/architecture.md section 7.1) and the node clock:
// single-threaded edge cases, then a producer and a consumer thread checking every value and
// record arrives once, in order, intact (run under the tsan preset too).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "jarvis/live/clock.hpp"
#include "jarvis/live/spsc_ring.hpp"

namespace {

namespace live = jarvis::live;

std::uint64_t mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

// Record i: its size from the index, bytes a pattern of the index.
std::size_t record_size(std::uint64_t i, std::size_t max) { return mix(i) % (max + 1); }
std::byte pattern(std::uint64_t i, std::size_t k) {
  return static_cast<std::byte>((i * 31U + k) & 0xFFU);
}

} // namespace

TEST_SUITE("unit") {
  TEST_CASE("the typed ring holds its capacity, refuses more, and keeps order across wraps") {
    live::SpscRing<std::uint64_t> ring{5};
    CHECK(ring.capacity() == 8);
    for (std::uint64_t round = 0; round < 3; ++round) {
      for (std::uint64_t i = 0; i < 8; ++i) {
        CHECK(ring.try_push(round * 100 + i));
      }
      CHECK_FALSE(ring.try_push(999));
      CHECK(ring.size() == 8);
      for (std::uint64_t i = 0; i < 8; ++i) {
        std::uint64_t v = 0;
        REQUIRE(ring.try_pop(v));
        CHECK(v == round * 100 + i);
      }
      std::uint64_t v = 0;
      CHECK_FALSE(ring.try_pop(v));
      CHECK(ring.front() == nullptr);
    }
  }

  TEST_CASE("the byte ring wraps records whole and refuses what cannot fit") {
    live::SpscByteRing ring{256};
    CHECK(ring.capacity() == 256);
    CHECK(ring.max_record() == 120);
    CHECK(ring.reserve(121).empty());
    bool empty = false;
    CHECK(ring.peek(empty).empty());
    CHECK(empty);

    // 100-byte records (112 with the header and padding): two fit, the third must wait, and
    // after one is released it wraps to the start of the buffer.
    std::vector<std::byte> bytes(100);
    for (std::uint64_t i = 0; i < 20; ++i) {
      for (std::size_t k = 0; k < bytes.size(); ++k) {
        bytes[k] = pattern(i, k);
      }
      if (!ring.try_write(bytes)) {
        const std::span<const std::byte> r = ring.peek(empty);
        REQUIRE_FALSE(empty);
        CHECK(r.size() == 100);
        ring.release();
        REQUIRE(ring.try_write(bytes));
      }
    }
    std::uint64_t seen = 0;
    for (;;) {
      const std::span<const std::byte> r = ring.peek(empty);
      if (empty) {
        break;
      }
      CHECK(r.size() == 100);
      ring.release();
      ++seen;
    }
    CHECK(seen == 2);
    CHECK(ring.used() == 0);

    // A zero-length record and a reservation shortened at commit.
    CHECK(ring.try_write({}));
    const std::span<std::byte> room = ring.reserve(64);
    REQUIRE(room.size() == 64);
    room[0] = std::byte{7};
    ring.commit(1);
    CHECK(ring.peek(empty).empty());
    CHECK_FALSE(empty);
    ring.release();
    const std::span<const std::byte> one = ring.peek(empty);
    REQUIRE(one.size() == 1);
    CHECK(one[0] == std::byte{7});
    ring.release();
  }

  TEST_CASE("a producer and a consumer thread pass a million values in order") {
    constexpr std::uint64_t kCount = 1'000'000;
    live::SpscRing<std::uint64_t> ring{1024};
    std::thread producer{[&] {
      for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!ring.try_push(mix(i))) {
          std::this_thread::yield();
        }
      }
    }};
    std::uint64_t mismatches = 0;
    for (std::uint64_t i = 0; i < kCount;) {
      std::uint64_t v = 0;
      if (ring.try_pop(v)) {
        mismatches += v == mix(i) ? 0U : 1U;
        ++i;
      } else {
        std::this_thread::yield();
      }
    }
    producer.join();
    CHECK(mismatches == 0);
  }

  TEST_CASE("variable records cross threads intact, wrapping many times") {
    constexpr std::uint64_t kCount = 200'000;
    live::SpscByteRing ring{1U << 14U};
    const std::size_t max = 3000;
    std::thread producer{[&] {
      std::vector<std::byte> bytes;
      for (std::uint64_t i = 0; i < kCount; ++i) {
        const std::size_t n = record_size(i, max);
        std::span<std::byte> room;
        while ((room = ring.reserve(n + 8)).empty()) {
          std::this_thread::yield();
        }
        std::memcpy(room.data(), &i, 8);
        for (std::size_t k = 0; k < n; ++k) {
          room[8 + k] = pattern(i, k);
        }
        ring.commit(n + 8);
      }
    }};
    std::uint64_t bad = 0;
    for (std::uint64_t i = 0; i < kCount;) {
      bool empty = false;
      const std::span<const std::byte> r = ring.peek(empty);
      if (empty) {
        std::this_thread::yield();
        continue;
      }
      std::uint64_t index = 0;
      std::memcpy(&index, r.data(), 8);
      const std::size_t n = record_size(i, max);
      bool ok = index == i && r.size() == n + 8;
      for (std::size_t k = 0; ok && k < n; ++k) {
        ok = r[8 + k] == pattern(i, k);
      }
      bad += ok ? 0U : 1U;
      ring.release();
      ++i;
    }
    producer.join();
    CHECK(bad == 0);
  }

  TEST_CASE("the node clock is UTC and never goes backwards") {
    const live::ArrivalClock anchor;
    const live::MonotonicClock clock{anchor};
    jarvis::core::UnixNanos last = clock.now();
    CHECK(last.value() > 1'700'000'000'000'000'000ULL); // after 2023
    for (int i = 0; i < 10'000; ++i) {
      const jarvis::core::UnixNanos t = clock.now();
      CHECK_FALSE(t < last);
      last = t;
    }
  }
}
