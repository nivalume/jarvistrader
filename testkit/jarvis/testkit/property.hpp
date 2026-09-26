#pragma once

#include <cstdint>
#include <cstdlib>

#include <doctest/doctest.h>

#include "jarvis/testkit/gen.hpp"

namespace jarvis::testkit {

inline constexpr std::uint64_t kDefaultPropertySeed = 0x6A61727669730000ULL;
inline constexpr std::uint64_t kDefaultPropertyIterations = 256;

struct PropertyConfig {
  std::uint64_t seed;
  std::uint64_t iterations;
  bool single_case;
  std::uint64_t case_index;
};

namespace detail {

inline bool read_env_u64(const char* name, std::uint64_t& out) noexcept {
  const char* text = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read once per test
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

} // namespace detail

// JARVIS_PROP_SEED selects the base seed, JARVIS_PROP_ITERS the number of cases, and
// JARVIS_PROP_CASE runs exactly one case so that a reported failure can be replayed.
inline PropertyConfig property_config_from_env() noexcept {
  PropertyConfig config{kDefaultPropertySeed, kDefaultPropertyIterations, false, 0};
  static_cast<void>(detail::read_env_u64("JARVIS_PROP_SEED", config.seed));
  static_cast<void>(detail::read_env_u64("JARVIS_PROP_ITERS", config.iterations));
  config.single_case = detail::read_env_u64("JARVIS_PROP_CASE", config.case_index);
  return config;
}

// Seed of case `index` under base seed `seed`. Cases are independent of each other, so a single
// case can be rerun without replaying the ones before it.
[[nodiscard]] inline std::uint64_t case_seed(std::uint64_t seed, std::uint64_t index) noexcept {
  Gen mixer{seed ^ (index * 0x9E3779B97F4A7C15ULL)};
  return mixer.next();
}

// Runs `body(Gen&)` once per case. Failures report the base seed and case index.
template <typename Body> void for_all(Body&& body) {
  const PropertyConfig config = property_config_from_env();
  const std::uint64_t first = config.single_case ? config.case_index : 0U;
  const std::uint64_t last = config.single_case ? config.case_index + 1U : config.iterations;
  for (std::uint64_t index = first; index < last; ++index) {
    DOCTEST_INFO("property case: rerun with JARVIS_PROP_SEED=", config.seed,
                 " JARVIS_PROP_CASE=", index);
    Gen gen{case_seed(config.seed, index)};
    body(gen);
  }
}

} // namespace jarvis::testkit
