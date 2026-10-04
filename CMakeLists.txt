cmake_minimum_required(VERSION 3.25)

project(jarvis VERSION 0.0.0 LANGUAGES CXX)

# The kernel computes in __int128, which cl.exe lacks. On Windows use clang-cl (the MSVC ABI, so
# python.org CPython loads the extension) or MinGW-w64 GCC/Clang; docs/development.md section 6.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  message(FATAL_ERROR "jarvis requires GCC, Clang, AppleClang, or clang-cl; cl.exe is not "
                      "supported. On Windows configure with -T ClangCL or the win-* presets.")
endif()
# clang-cl takes MSVC-style options (/W4, /WX, /Od); GCC, Clang, AppleClang and MinGW take GNU ones.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(JARVIS_MSVC_FRONTEND ON)
else()
  set(JARVIS_MSVC_FRONTEND OFF)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    message(FATAL_ERROR "jarvis requires GCC 13 or newer")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    message(FATAL_ERROR "jarvis requires Clang 17 or newer")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
    message(FATAL_ERROR "jarvis requires AppleClang 15 or newer")
  endif()
else()
  message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.15 CACHE STRING "Minimum policy version accepted for pinned dependencies")

option(JARVIS_BUILD_PYTHON "Build the Python extension" ON)
option(JARVIS_BUILD_TESTS "Configure the C++ test scaffold" ${PROJECT_IS_TOP_LEVEL})
option(JARVIS_BUILD_BENCHMARKS "Configure the benchmark scaffold" OFF)
option(JARVIS_BUILD_CLI "Build the jarvis command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(JARVIS_BUILD_LIVE "Build live-trading shell components (network stack, venue adapters)" OFF)
option(JARVIS_BUILD_EXAMPLES "Build the example strategies" ${PROJECT_IS_TOP_LEVEL})
option(JARVIS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JARVIS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(JARVIS_ENABLE_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(JARVIS_FORCE_O0 "Force first-party targets to compile without optimization" OFF)

if(WIN32 AND (JARVIS_ENABLE_SANITIZERS OR JARVIS_ENABLE_TSAN OR JARVIS_ENABLE_FUZZ))
  message(FATAL_ERROR "the sanitizer and fuzz builds run on Linux and macOS only")
endif()
if(JARVIS_ENABLE_TSAN AND (JARVIS_ENABLE_SANITIZERS OR JARVIS_ENABLE_FUZZ))
  message(FATAL_ERROR "JARVIS_ENABLE_TSAN cannot be combined with ASan/UBSan or fuzzing")
endif()
if(JARVIS_ENABLE_FUZZ AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "JARVIS_ENABLE_FUZZ requires Clang (libFuzzer)")
endif()

# Kernel layers, lowest first. Must match docs/architecture.md section 3 and tools/check-layering.py.
set(JARVIS_KERNEL_LAYERS core model data cost portfolio execution risk strategy engine backtest)

add_library(jarvis_strict INTERFACE)
add_library(jarvis::strict ALIAS jarvis_strict)
target_compile_features(jarvis_strict INTERFACE cxx_std_20)
if(JARVIS_MSVC_FRONTEND)
  # clang-cl reads -Wall as -Weverything, so the warning level comes from /W4 (-Wall -Wextra).
  # The GNU -f options pass through /clang:.
  target_compile_options(
    jarvis_strict
    INTERFACE
      /W4
      /WX
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wswitch-enum
      -Wnon-virtual-dtor
      -Wold-style-cast
      /clang:-ffp-contract=off
      /clang:-fno-fast-math
      $<$<BOOL:${JARVIS_FORCE_O0}>:/Od>
      $<$<CONFIG:Debug>:/Oy->
  )
else()
  target_compile_options(
    jarvis_strict
    INTERFACE
      -Wall
      -Wextra
      -Werror
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wswitch-enum
      -Wnon-virtual-dtor
      -Wold-style-cast
      -ffp-contract=off
      -fno-fast-math
      $<$<BOOL:${JARVIS_FORCE_O0}>:-O0>
      $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
  )
endif()

if(APPLE)
  target_compile_definitions(
    jarvis_strict
    INTERFACE $<$<CONFIG:Debug>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>
  )
elseif(NOT JARVIS_MSVC_FRONTEND) # the MSVC library checks iterators in Debug by itself
  target_compile_definitions(jarvis_strict INTERFACE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
endif()

if(WIN32)
  # Windows 10 1803 or newer (AF_UNIX sockets); no min/max macros from <windows.h>; the CRT's
  # "use the _s variant" deprecations are not warnings. The third-party code needs the same.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
  # The test executables pass the COFF limit of 65536 sections per object file.
  if(JARVIS_MSVC_FRONTEND)
    add_compile_options(/bigobj)
  else()
    add_compile_options(-Wa,-mbig-obj)
  endif()
endif()

if(JARVIS_MSVC_FRONTEND)
  # The MSVC runtime has no __int128 division and multiplication helpers (__divti3, __muloti4)
  # nor the CPU feature table behind __builtin_cpu_supports; clang ships them in compiler-rt.
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /clang:--rtlib=compiler-rt /clang:-print-libgcc-file-name
    OUTPUT_VARIABLE JARVIS_CLANG_BUILTINS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT JARVIS_CLANG_BUILTINS OR NOT EXISTS "${JARVIS_CLANG_BUILTINS}")
    # LLVM releases differ in where they keep it: lib/windows/clang_rt.builtins-x86_64.lib or
    # lib/x86_64-pc-windows-msvc/clang_rt.builtins.lib under the resource directory.
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
      OUTPUT_VARIABLE _jarvis_clang_resource
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    file(GLOB _jarvis_builtins "${_jarvis_clang_resource}/lib/windows/clang_rt.builtins-x86_64.lib"
         "${_jarvis_clang_resource}/lib/x86_64-pc-windows-msvc/clang_rt.builtins.lib")
    if(_jarvis_builtins)
      list(GET _jarvis_builtins 0 JARVIS_CLANG_BUILTINS)
    endif()
  endif()
  if(NOT JARVIS_CLANG_BUILTINS OR NOT EXISTS "${JARVIS_CLANG_BUILTINS}")
    message(FATAL_ERROR "cannot find clang's compiler-rt builtins library (clang_rt.builtins); "
                        "${CMAKE_CXX_COMPILER} printed \"${JARVIS_CLANG_BUILTINS}\"")
  endif()
  message(STATUS "clang-cl builtins: ${JARVIS_CLANG_BUILTINS}")
  link_libraries("${JARVIS_CLANG_BUILTINS}")
endif()

if(JARVIS_ENABLE_SANITIZERS)
  target_compile_options(jarvis_strict INTERFACE -fsanitize=address,undefined)
  target_link_options(jarvis_strict INTERFACE -fsanitize=address,undefined)
endif()

if(JARVIS_ENABLE_TSAN)
  target_compile_options(jarvis_strict INTERFACE -fsanitize=thread)
  target_link_options(jarvis_strict INTERFACE -fsanitize=thread)
endif()

if(JARVIS_ENABLE_FUZZ)
  # Instrument first-party code for coverage; only fuzz executables link the libFuzzer driver.
  target_compile_options(jarvis_strict INTERFACE -fsanitize=fuzzer-no-link,address,undefined)
  target_link_options(jarvis_strict INTERFACE -fsanitize=address,undefined)
endif()

add_library(jarvis_kernel INTERFACE)
add_library(jarvis::kernel ALIAS jarvis_kernel)
target_link_libraries(jarvis_kernel INTERFACE jarvis::strict)
target_include_directories(
  jarvis_kernel
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# Freestanding guard: one generated translation unit per kernel header, compiled without
# exceptions and RTTI. New headers are picked up automatically, and each header must be
# self-contained because it is compiled on its own.
set(_jarvis_kernel_headers)
foreach(layer IN LISTS JARVIS_KERNEL_LAYERS)
  file(GLOB_RECURSE _layer_headers CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/jarvis/${layer}/*.hpp")
  list(APPEND _jarvis_kernel_headers ${_layer_headers})
endforeach()
list(SORT _jarvis_kernel_headers)
set(_jarvis_freestanding_sources testkit/freestanding.cpp)
foreach(header IN LISTS _jarvis_kernel_headers)
  file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${header}")
  string(REGEX REPLACE "[/.]" "_" _stem "${_rel}")
  set(_tu "${CMAKE_CURRENT_BINARY_DIR}/generated/freestanding/${_stem}.cpp")
  file(CONFIGURE OUTPUT "${_tu}"
       CONTENT "// Generated by CMakeLists.txt; do not edit.\n#include \"${_rel}\"\n")
  list(APPEND _jarvis_freestanding_sources "${_tu}")
endforeach()
add_library(jarvis_kernel_freestanding OBJECT ${_jarvis_freestanding_sources})
target_link_libraries(jarvis_kernel_freestanding PRIVATE jarvis::kernel)
if(JARVIS_MSVC_FRONTEND)
  # The MSVC library throws unless _HAS_EXCEPTIONS is 0.
  target_compile_options(jarvis_kernel_freestanding PRIVATE /EHs-c- /GR-)
  target_compile_definitions(jarvis_kernel_freestanding PRIVATE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(jarvis_kernel_freestanding PRIVATE -fno-exceptions -fno-rtti)
endif()

include(cmake/Dependencies.cmake)

# Shell: code that may use exceptions and third-party libraries internally and translates
# failures into Status at the kernel boundary. Live networking is added only with
# JARVIS_BUILD_LIVE so a backtest-only build never needs the network stack.
execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE JARVIS_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT JARVIS_GIT_COMMIT)
  set(JARVIS_GIT_COMMIT "unknown")
endif()
if(JARVIS_BUILD_LIVE)
  set(JARVIS_LIVE_BOOL true)
else()
  set(JARVIS_LIVE_BOOL false)
endif()
set(JARVIS_PLATFORM "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
configure_file(jarvis/node/build_config.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/include/jarvis/node/build_config.hpp" @ONLY)

# Operating system calls (docs/architecture.md section 3): files written to survive a crash,
# sockets, shared libraries, stop requests. The other shell layers name no platform API.
add_library(
  jarvis_sys STATIC
  jarvis/sys/error.cpp
  jarvis/sys/file.cpp
  jarvis/sys/library.cpp
  jarvis/sys/process.cpp
  jarvis/sys/shutdown.cpp
  jarvis/sys/socket.cpp
  jarvis/sys/time.cpp
)
add_library(jarvis::sys ALIAS jarvis_sys)
target_link_libraries(jarvis_sys PUBLIC jarvis::kernel)
if(WIN32)
  target_link_libraries(jarvis_sys PRIVATE ws2_32)
else()
  target_link_libraries(jarvis_sys PRIVATE ${CMAKE_DL_LIBS})
endif()

add_library(
  jarvis_shell STATIC
  jarvis/node/build_info.cpp
  jarvis/node/corpus.cpp
  jarvis/node/event_log.cpp
  jarvis/node/event_text.cpp
  jarvis/node/fingerprint.cpp
  jarvis/node/model_text.cpp
  jarvis/node/config.cpp
  jarvis/node/admin_protocol.cpp
  jarvis/node/credentials.cpp
  jarvis/node/epoch_store.cpp
  jarvis/node/toml_impl.cpp
  jarvis/node/strategy_registry.cpp
  jarvis/node/strategy_plugin.cpp
  jarvis/node/log_source.cpp
  jarvis/node/run_dir.cpp
  jarvis/node/snapshot_file.cpp
  jarvis/node/state_text.cpp
  jarvis/node/backtest_node.cpp
  jarvis/node/node_cli.cpp
  jarvis/node/trace_export.cpp
  jarvis/node/order_replay.cpp
  jarvis/node/run_report.cpp
)
add_library(jarvis::shell ALIAS jarvis_shell)
target_link_libraries(jarvis_shell PUBLIC jarvis::kernel jarvis::sys)
include(cmake/StrategyPlugin.cmake)
target_include_directories(jarvis_shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
jarvis_add_tomlplusplus()
# toml++ stays private to the shell: headers that other targets include never expose it.
target_link_libraries(jarvis_shell PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(jarvis_shell PRIVATE TOML_HEADER_ONLY=0 TOML_EXCEPTIONS=0 TOML_ENABLE_FORMATTERS=0)

# Live shell (docs/architecture.md section 13): the network stack. Asio, OpenSSL and
# picohttpparser stay private to it: the headers other layers include name only the standard
# library and jarvis types (a static library still passes its private link dependencies on).
if(JARVIS_BUILD_LIVE)
  enable_language(C) # picohttpparser
  find_package(OpenSSL 3 REQUIRED)
  jarvis_add_asio()
  jarvis_add_picohttpparser()
  add_library(
    jarvis_network STATIC
    jarvis/network/crypto.cpp
    jarvis/network/http.cpp
    jarvis/network/https_client.cpp
    jarvis/network/io.cpp
    jarvis/network/signer.cpp
    jarvis/network/timer.cpp
    jarvis/network/waker.cpp
    jarvis/network/url.cpp
    jarvis/network/ws_client.cpp
    jarvis/network/ws_frame.cpp
  )
  add_library(jarvis::network ALIAS jarvis_network)
  target_link_libraries(
    jarvis_network
    PUBLIC jarvis::kernel
    PRIVATE jarvis::sys jarvis_asio jarvis_picohttpparser OpenSSL::SSL OpenSSL::Crypto
  )

  # Venue adapters (docs/architecture.md section 14): codecs, instrument loading.
  jarvis_add_simdjson()
  add_library(
    jarvis_adapter STATIC
    jarvis/adapter/codec.cpp
    jarvis/adapter/binance/exchange_info.cpp
    jarvis/adapter/binance/json_codec.cpp
    jarvis/adapter/binance/order_tracker.cpp
    jarvis/adapter/binance/requests.cpp
    jarvis/adapter/binance/rest_client.cpp
    jarvis/adapter/binance/rest_codec.cpp
    jarvis/adapter/binance/snapshot.cpp
    jarvis/adapter/binance/startup.cpp
    jarvis/adapter/binance/streams.cpp
    jarvis/adapter/binance/user_stream.cpp
    jarvis/adapter/binance/user_stream_session.cpp
    jarvis/adapter/binance/ws_api.cpp
  )
  add_library(jarvis::adapter ALIAS jarvis_adapter)
  target_link_libraries(
    jarvis_adapter
    PUBLIC jarvis::kernel jarvis::network
    PRIVATE simdjson::simdjson
  )

  # Live runtime pieces (docs/architecture.md sections 7 and 13.4).
  add_library(jarvis_live STATIC jarvis/live/raw_frames.cpp jarvis/live/market_feed.cpp
                          jarvis/live/redecode.cpp jarvis/live/sandbox_node.cpp
                          jarvis/live/venue_io.cpp jarvis/live/live_node.cpp
                          jarvis/live/admin_server.cpp jarvis/live/persist.cpp
                          jarvis/live/telemetry.cpp jarvis/live/cpu_affinity.cpp)
  add_library(jarvis::live ALIAS jarvis_live)
  target_link_libraries(jarvis_live PUBLIC jarvis::kernel jarvis::shell jarvis::network jarvis::adapter)

  add_executable(jarvis_capture jarvis/live/capture_main.cpp)
  target_link_libraries(jarvis_capture PRIVATE jarvis::live)
  set_target_properties(jarvis_capture PROPERTIES OUTPUT_NAME jarvis-capture
                                                  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(JARVIS_BUILD_CLI)
  add_executable(jarvis_cli jarvis/node/cli_main.cpp)
  target_link_libraries(jarvis_cli PRIVATE jarvis::shell)
  set_target_properties(
    jarvis_cli PROPERTIES OUTPUT_NAME jarvis RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
endif()

add_library(jarvis_testkit INTERFACE)
add_library(jarvis::testkit ALIAS jarvis_testkit)
target_include_directories(jarvis_testkit INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/testkit")
target_link_libraries(jarvis_testkit INTERFACE jarvis::strict)

if(JARVIS_BUILD_PYTHON)
  find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
  jarvis_add_nanobind()
  # NOMINSIZE: nanobind builds with -Os by default; the kernel templates instantiated here (the
  # engine and the backtest driver behind jarvis.Node) run at -O3 like the rest of the build.
  # On Windows nanobind's headers are system headers: their MSVC paths are not held to the
  # project's warning set.
  set(_jarvis_nanobind_options NOMINSIZE)
  if(WIN32)
    list(APPEND _jarvis_nanobind_options NB_SUPPRESS_WARNINGS)
  endif()
  nanobind_add_module(
    _core
    ${_jarvis_nanobind_options}
    python/src/bindings.cpp
    python/src/bind_values.cpp
    python/src/bind_events.cpp
    python/src/bind_log.cpp
    python/src/bind_node.cpp
    python/src/generated/enums.cpp
  )
  target_include_directories(
    _core
    SYSTEM PRIVATE
      ${Python_INCLUDE_DIRS}
      ${nanobind_SOURCE_DIR}/include
  )
  target_link_libraries(_core PRIVATE jarvis::shell)
  # With the live shell, jarvis.Node also runs sandbox sessions (node.env = "sandbox").
  if(TARGET jarvis_live)
    target_link_libraries(_core PRIVATE jarvis::live)
    target_compile_definitions(_core PRIVATE JARVIS_PY_LIVE=1)
  endif()
  install(TARGETS _core LIBRARY DESTINATION jarvis RUNTIME DESTINATION jarvis)
endif()

if(JARVIS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(JARVIS_BUILD_TESTS)
  include(CTest)
  jarvis_add_doctest()
  add_custom_target(jarvis_tests)
  add_subdirectory(tests)
endif()

if(JARVIS_BUILD_BENCHMARKS)
  jarvis_add_benchmark()
  add_custom_target(jarvis_bench)
  add_subdirectory(benchmarks)
endif()

if(JARVIS_ENABLE_FUZZ)
  add_custom_target(jarvis_fuzz)
  add_subdirectory(tests/fuzz)
endif()
