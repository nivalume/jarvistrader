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
- Arenas and integer index handles replace persistent object pointers. The order handle type is
  `OrderHandle`: a 32-bit arena index plus a generation counter. It is deliberately not called
  `OrderId`, because the nautilus-compatible model uses that name for the venue's 64-bit order ID.
- Capacities come from configuration and are fixed at startup. A full container returns
  `Status::CapacityExceeded` instead of growing.
- Fallible kernel operations return an `enum class Status` and write successful values through an
  out parameter. They do not throw.
- Data is `const` unless mutation is required by the state transition.

## Hot path

The hot path is everything between dequeuing a market-data or venue event and enqueuing the
resulting order command: routing, strategy callbacks, the execution algorithm, both risk gates,
and the OMS. On the hot path:

- Dispatch is static: concept-constrained templates, or a closed `std::variant` visited with
  `std::visit`. There are no virtual calls and no `std::function`.
- The one sanctioned indirect call is the function-pointer table (`StrategyVTable`) of
  `DynamicStrategySet`, which a Python-launched node needs because its strategy set is unknown at
  compile time (docs/architecture.md section 7.3). Pure C++ nodes use `StaticStrategySet` and make
  no indirect calls.
- Nothing allocates. The zero-allocation gate runs every kernel step in the test suite under a
  counting global `operator new` and fails on any allocation (ctest label `zero-alloc`).

Virtual interfaces remain acceptable off the hot path: startup configuration, portfolio
construction on timers, post-trade monitors, and the Python strategy host.

## Build enforcement

Every first-party C++ target links `jarvis::strict`. Every kernel header is compiled on its own
through `jarvis_kernel_freestanding`, which generates one translation unit per header and disables
exceptions and RTTI, so headers must be self-contained and exception-free. `tools/check-layering.py`
enforces the include direction between layers and bans threading, clock, stream, random and
third-party headers from the kernel. clang-tidy and compiler warnings are errors; `-Wconversion`
and `-Wsign-conversion` must not be disabled locally.
