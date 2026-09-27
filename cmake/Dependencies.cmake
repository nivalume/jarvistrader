include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

function(jarvis_add_nanobind)
  if(TARGET nanobind-static)
    return()
  endif()

  # nanobind v2.14.0
  CPMAddPackage(
    NAME nanobind
    GITHUB_REPOSITORY wjakob/nanobind
    GIT_TAG 86e5626728fc282637a6c338597120913dded1bb
    EXCLUDE_FROM_ALL YES
  )
endfunction()

function(jarvis_add_doctest)
  if(TARGET doctest::doctest)
    return()
  endif()

  # doctest v2.5.3
  CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG 2d0a9359a60c51affe2a9bebb1be1dca47868151
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
  )
endfunction()

function(jarvis_add_benchmark)
  if(TARGET benchmark::benchmark)
    return()
  endif()

  # google/benchmark v1.9.5
  CPMAddPackage(
    NAME benchmark
    GITHUB_REPOSITORY google/benchmark
    GIT_TAG 192ef10025eb2c4cdd392bc502f0c852196baa48
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS
      "BENCHMARK_ENABLE_TESTING OFF"
      "BENCHMARK_ENABLE_GTEST_TESTS OFF"
      "BENCHMARK_ENABLE_INSTALL OFF"
  )
endfunction()

function(jarvis_add_tomlplusplus)
  if(TARGET tomlplusplus::tomlplusplus)
    return()
  endif()

  # toml++ master after v3.4.0 (header-only; compiled once in jarvis/node/toml_impl.cpp).
  # v3.4.0 reaches TOML_UNREACHABLE in is_non_ascii_horizontal_whitespace for characters such
  # as U+3002 after a value (found by tests/fuzz/fuzz_config); master returns false instead.
  CPMAddPackage(
    NAME tomlplusplus
    GITHUB_REPOSITORY marzer/tomlplusplus
    GIT_TAG 1e8829b793b66ad17011732a146b8077d379b011
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
  )
endfunction()

# ---- live shell (JARVIS_BUILD_LIVE) -------------------------------------------------------------

function(jarvis_add_asio)
  if(TARGET jarvis_asio)
    return()
  endif()

  # standalone Asio 1.38.2 (header-only; no Boost).
  CPMAddPackage(
    NAME asio
    GITHUB_REPOSITORY chriskohlhoff/asio
    GIT_TAG 12b52a54a25d1861b037bc5d9810f92a7151d920
    DOWNLOAD_ONLY YES
  )
  find_package(Threads REQUIRED)
  add_library(jarvis_asio INTERFACE)
  target_include_directories(jarvis_asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
  target_compile_definitions(jarvis_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  target_link_libraries(jarvis_asio INTERFACE Threads::Threads)
endfunction()

function(jarvis_add_picohttpparser)
  if(TARGET jarvis_picohttpparser)
    return()
  endif()

  # picohttpparser master (HTTP response and chunked-encoding parser; one C file). The caller
  # enables the C language at directory scope first.
  CPMAddPackage(
    NAME picohttpparser
    GITHUB_REPOSITORY h2o/picohttpparser
    GIT_TAG f4d94b48b31e0abae029ebeafcfd9ca0680ede58
    DOWNLOAD_ONLY YES
  )
  add_library(jarvis_picohttpparser STATIC "${picohttpparser_SOURCE_DIR}/picohttpparser.c")
  target_include_directories(jarvis_picohttpparser SYSTEM PUBLIC "${picohttpparser_SOURCE_DIR}")
  set_target_properties(jarvis_picohttpparser PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(jarvis_add_simdjson)
  if(TARGET simdjson::simdjson)
    return()
  endif()

  # simdjson v4.6.11 (the venue adapters' JSON codec).
  CPMAddPackage(
    NAME simdjson
    GITHUB_REPOSITORY simdjson/simdjson
    GIT_TAG f5de14f09256982933af2849beb43778bd421ca7
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
    OPTIONS
      "SIMDJSON_DEVELOPER_MODE OFF"
      "SIMDJSON_ENABLE_THREADS OFF"
      "BUILD_SHARED_LIBS OFF"
  )
endfunction()
