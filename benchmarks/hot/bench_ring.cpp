// Gating benchmarks for the live rings (docs/architecture.md sections 7.1 and 17.3).
//
//   ring/spsc_roundtrip   one value to an echo thread and back through two SpscRings: twice
//                         the one-way hand-off latency between cores, cache-line transfers
//                         included
//   ring/byte_record      one 256-byte record written and read on one thread (the copy and
//                         index bookkeeping of the byte ring, no cross-core traffic)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "jarvis/live/spsc_ring.hpp"

namespace {

namespace live = jarvis::live;

void spsc_roundtrip(benchmark::State& state) {
  live::SpscRing<std::uint64_t> out{1024};
  live::SpscRing<std::uint64_t> back{1024};
  std::atomic<bool> stop{false};
  std::thread echo{[&] {
    std::uint64_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (out.try_pop(v)) {
        while (!back.try_push(v)) {
        }
      }
    }
  }};
  std::uint64_t i = 0;
  for ([[maybe_unused]] auto _ : state) {
    while (!out.try_push(++i)) {
    }
    std::uint64_t v = 0;
    while (!back.try_pop(v)) {
    }
    benchmark::DoNotOptimize(v);
  }
  stop = true;
  echo.join();
}
BENCHMARK(spsc_roundtrip)->Name("ring/spsc_roundtrip");

void byte_record(benchmark::State& state) {
  live::SpscByteRing ring{1U << 16U};
  std::vector<std::byte> record(256, std::byte{1});
  for ([[maybe_unused]] auto _ : state) {
    bool written = ring.try_write(record);
    bool empty = false;
    const std::byte* r = ring.peek(empty).data();
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(r);
    ring.release();
  }
}
BENCHMARK(byte_record)->Name("ring/byte_record");

} // namespace
