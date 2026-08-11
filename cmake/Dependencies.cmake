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
    OPTIONS
      "BENCHMARK_ENABLE_TESTING OFF"
      "BENCHMARK_ENABLE_GTEST_TESTS OFF"
      "BENCHMARK_ENABLE_INSTALL OFF"
  )
endfunction()
