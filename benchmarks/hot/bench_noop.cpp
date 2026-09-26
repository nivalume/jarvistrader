// Pipeline check for the benchmark gate: a fixed amount of trivial work, so the A/B comparison,
// the thresholds file and the CI job can be exercised before real kernel benchmarks exist.
// Each iteration performs kSteps dependent increments that the optimizer cannot remove, which
// keeps the measured time well above the absolute noise floor in thresholds.toml.

#include <cstdint>

#include <benchmark/benchmark.h>

namespace {

constexpr int kSteps = 64;

void noop_loop(benchmark::State& state) {
  std::uint64_t counter = 0;
  for ([[maybe_unused]] auto iteration : state) {
    for (int step = 0; step < kSteps; ++step) {
      counter += 1;
      benchmark::DoNotOptimize(counter);
    }
  }
}

} // namespace

BENCHMARK(noop_loop)->Name("noop/loop");
