# C++ subset for the kernel

The kernel uses C++20 with a deliberately small, explicit subset. These rules apply to first-party
kernel code. Boundary code may use mechanisms required by nanobind, but it must not leak those
mechanisms into kernel headers.

## Forbidden in the kernel

- exceptions, RTTI, and `dynamic_cast`
- virtual dispatch on hot paths
- implementation inheritance; pure interfaces are the only permitted inheritance
- `std::shared_ptr` and raw `new`/`delete`
- SFINAE and template metaprogramming
- operator overloads other than comparisons and indexing
- implicit converting constructors; single-argument constructors must be `explicit`
- iostreams
- macros other than platform detection
- `std::function` on hot paths
- stateful random generators such as `std::mt19937`

## Permitted

- structs, member functions, RAII, and const-correct value semantics
- `std::vector`, `std::array`, `std::span`, `std::optional`, and `std::string_view`
- `enum class`, `constexpr`, `[[nodiscard]]`, structured bindings, and range-for
- simple templates constrained with concepts

## Ownership and error model

- One owner stores objects in vectors; other code borrows them through spans.
- Arenas and integer index handles replace persistent object pointers. The planned `OrderId` type is
  a 32-bit unsigned integer.
- Fallible kernel operations return an `enum class Status` and write successful values through an
  out parameter. They do not throw.
- Data is `const` unless mutation is required by the state transition.

## Build enforcement

Every first-party C++ target links `jarvis::strict`. Kernel headers must also compile through
`jarvis_kernel_freestanding`, which disables exceptions and RTTI. clang-tidy and compiler warnings
are errors; `-Wconversion` and `-Wsign-conversion` must not be disabled locally.
