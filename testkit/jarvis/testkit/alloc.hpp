#pragma once

#include <cstdint>

namespace jarvis::testkit {

// Number of global operator new calls made by the calling thread so far. Only meaningful in
// executables that link the jarvis_testkit_alloc object library, which replaces the global
// allocation functions with counting versions.
[[nodiscard]] std::uint64_t thread_allocation_count() noexcept;

// Counts allocations made by the current thread while the scope is alive. The zero-allocation
// gate asserts that `allocations()` is zero after running a kernel step
// (docs/architecture.md section 17.2).
class AllocationScope {
public:
  AllocationScope() noexcept : start_{thread_allocation_count()} {}

  [[nodiscard]] std::uint64_t allocations() const noexcept {
    return thread_allocation_count() - start_;
  }

private:
  std::uint64_t start_;
};

} // namespace jarvis::testkit
