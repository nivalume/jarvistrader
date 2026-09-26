// Replaces the global allocation functions with versions that count calls per thread.
// Linked only into test executables through the jarvis_testkit_alloc object library.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "jarvis/testkit/alloc.hpp"

namespace {

thread_local std::uint64_t g_allocations = 0;

void* allocate(std::size_t size) {
  ++g_allocations;
  void* ptr = std::malloc(size == 0 ? 1 : size); // NOLINT(cppcoreguidelines-no-malloc)
  if (ptr == nullptr) {
    throw std::bad_alloc{};
  }
  return ptr;
}

void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
  ++g_allocations;
  const auto align = static_cast<std::size_t>(alignment);
  const std::size_t rounded = size == 0 ? align : (size + align - 1) / align * align;
  void* ptr = std::aligned_alloc(align, rounded);
  if (ptr == nullptr) {
    throw std::bad_alloc{};
  }
  return ptr;
}

void* allocate_nothrow(std::size_t size) noexcept {
  ++g_allocations;
  return std::malloc(size == 0 ? 1 : size); // NOLINT(cppcoreguidelines-no-malloc)
}

void release(void* ptr) noexcept { std::free(ptr); } // NOLINT(cppcoreguidelines-no-malloc)

} // namespace

namespace jarvis::testkit {

std::uint64_t thread_allocation_count() noexcept { return g_allocations; }

} // namespace jarvis::testkit

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_aligned(size, alignment);
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return allocate_nothrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return allocate_nothrow(size);
}

void operator delete(void* ptr) noexcept { release(ptr); }
void operator delete[](void* ptr) noexcept { release(ptr); }
void operator delete(void* ptr, std::size_t /*size*/) noexcept { release(ptr); }
void operator delete[](void* ptr, std::size_t /*size*/) noexcept { release(ptr); }
void operator delete(void* ptr, std::align_val_t /*alignment*/) noexcept { release(ptr); }
void operator delete[](void* ptr, std::align_val_t /*alignment*/) noexcept { release(ptr); }
void operator delete(void* ptr, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  release(ptr);
}
void operator delete[](void* ptr, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  release(ptr);
}
void operator delete(void* ptr, const std::nothrow_t& /*tag*/) noexcept { release(ptr); }
void operator delete[](void* ptr, const std::nothrow_t& /*tag*/) noexcept { release(ptr); }
