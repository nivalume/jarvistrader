// Fuzz pipeline check: interprets the input as generator seeds and bounds and checks that
// Gen::below never returns a value outside its bound.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "jarvis/testkit/gen.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 16) {
    return 0;
  }
  std::uint64_t seed = 0;
  std::uint64_t bound = 0;
  std::memcpy(&seed, data, sizeof(seed));
  std::memcpy(&bound, data + sizeof(seed), sizeof(bound)); // NOLINT(*-pointer-arithmetic)
  if (bound == 0) {
    return 0;
  }
  jarvis::testkit::Gen gen{seed};
  if (gen.below(bound) >= bound) {
    __builtin_trap();
  }
  return 0;
}
