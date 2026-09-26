# Test executables. One doctest binary per layer keeps compile times down; each binary is
# registered once per doctest suite so that ctest labels select what runs
# (docs/architecture.md section 17.2).

find_package(Python 3.11 REQUIRED COMPONENTS Interpreter)

add_library(jarvis_testkit_main OBJECT "${PROJECT_SOURCE_DIR}/testkit/doctest_main.cpp")
target_link_libraries(jarvis_testkit_main PRIVATE jarvis::testkit doctest::doctest)

add_library(jarvis_testkit_alloc OBJECT "${PROJECT_SOURCE_DIR}/testkit/alloc_counter.cpp")
target_link_libraries(jarvis_testkit_alloc PRIVATE jarvis::testkit)

set(JARVIS_TEST_SUITES unit property conformance golden zero-alloc)
# Property suites run once per fixed seed, locally and in CI alike.
set(JARVIS_PROPERTY_SEEDS 1 2 3)

# jarvis_add_layer_test(<layer> SOURCES <files...> SUITES <suites...> [LIBS <targets...>])
function(jarvis_add_layer_test layer)
  cmake_parse_arguments(ARG "" "" "SOURCES;SUITES;LIBS" ${ARGN})
  if(NOT ARG_SOURCES OR NOT ARG_SUITES)
    message(FATAL_ERROR "jarvis_add_layer_test(${layer}) needs SOURCES and SUITES")
  endif()
  set(target "test_${layer}")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(
    ${target}
    PRIVATE jarvis::testkit doctest::doctest jarvis_testkit_main jarvis_testkit_alloc ${ARG_LIBS}
  )
  add_dependencies(jarvis_tests ${target})
  foreach(suite IN LISTS ARG_SUITES)
    if(NOT suite IN_LIST JARVIS_TEST_SUITES)
      message(FATAL_ERROR "Unknown test suite '${suite}' for ${layer}; allowed: ${JARVIS_TEST_SUITES}")
    endif()
    if(suite STREQUAL "property")
      foreach(seed IN LISTS JARVIS_PROPERTY_SEEDS)
        set(name "${layer}.property.seed${seed}")
        add_test(NAME ${name} COMMAND ${target} --test-suite=property)
        set_tests_properties(${name} PROPERTIES LABELS "property;${layer}"
                                                ENVIRONMENT "JARVIS_PROP_SEED=${seed}")
      endforeach()
    else()
      set(name "${layer}.${suite}")
      add_test(NAME ${name} COMMAND ${target} --test-suite=${suite})
      set_tests_properties(${name} PROPERTIES LABELS "${suite};${layer}")
    endif()
  endforeach()
endfunction()

jarvis_add_layer_test(testkit SOURCES cpp/test_testkit.cpp SUITES unit property zero-alloc)
jarvis_add_layer_test(core SOURCES cpp/test_core.cpp SUITES unit property zero-alloc LIBS jarvis::kernel)
jarvis_add_layer_test(model SOURCES cpp/test_model.cpp SUITES unit property zero-alloc LIBS jarvis::kernel)
jarvis_add_layer_test(node SOURCES cpp/test_node.cpp SUITES unit property LIBS jarvis::shell)

# Golden trace conformance (tests/golden/<case>/case.toml). Cases run the jarvis CLI.
if(NOT TARGET jarvis_cli)
  message(FATAL_ERROR "JARVIS_BUILD_TESTS needs JARVIS_BUILD_CLI for the golden cases")
endif()
add_dependencies(jarvis_tests jarvis_cli)
add_test(
  NAME golden.all
  COMMAND "${Python_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/golden.py"
          --root "${PROJECT_SOURCE_DIR}/tests/golden" --bin "${CMAKE_BINARY_DIR}"
)
set_tests_properties(golden.all PROPERTIES LABELS "golden")
