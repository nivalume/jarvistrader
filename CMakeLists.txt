cmake_minimum_required(VERSION 3.25)

project(jarvis VERSION 0.0.0 LANGUAGES CXX)

if(MSVC)
  message(FATAL_ERROR "jarvis requires GCC, Clang, or AppleClang; MSVC is not supported")
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
option(JARVIS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JARVIS_FORCE_O0 "Force first-party targets to compile without optimization" OFF)

add_library(jarvis_strict INTERFACE)
add_library(jarvis::strict ALIAS jarvis_strict)
target_compile_features(jarvis_strict INTERFACE cxx_std_20)
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

if(APPLE)
  target_compile_definitions(
    jarvis_strict
    INTERFACE $<$<CONFIG:Debug>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>
  )
else()
  target_compile_definitions(jarvis_strict INTERFACE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
endif()

if(JARVIS_ENABLE_SANITIZERS)
  target_compile_options(jarvis_strict INTERFACE -fsanitize=address,undefined)
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

add_library(jarvis_kernel_freestanding OBJECT testkit/freestanding.cpp)
target_link_libraries(jarvis_kernel_freestanding PRIVATE jarvis::kernel)
target_compile_options(jarvis_kernel_freestanding PRIVATE -fno-exceptions -fno-rtti)

include(cmake/Dependencies.cmake)

if(JARVIS_BUILD_PYTHON)
  find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
  jarvis_add_nanobind()
  nanobind_add_module(_core python/src/bindings.cpp)
  target_include_directories(
    _core
    SYSTEM PRIVATE
      ${Python_INCLUDE_DIRS}
      ${nanobind_SOURCE_DIR}/include
  )
  target_link_libraries(_core PRIVATE jarvis::kernel)
  install(TARGETS _core LIBRARY DESTINATION jarvis RUNTIME DESTINATION jarvis)
endif()

if(JARVIS_BUILD_TESTS)
  include(CTest)
  jarvis_add_doctest()
  add_custom_target(jarvis_tests)
endif()

if(JARVIS_BUILD_BENCHMARKS)
  jarvis_add_benchmark()
  add_custom_target(jarvis_bench)
  add_dependencies(jarvis_bench benchmark)
endif()
