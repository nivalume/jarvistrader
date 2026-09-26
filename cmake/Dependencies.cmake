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
