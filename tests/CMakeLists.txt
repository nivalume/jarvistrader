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
jarvis_add_layer_test(data SOURCES cpp/test_data.cpp SUITES unit property zero-alloc LIBS jarvis::kernel)
jarvis_add_layer_test(cost SOURCES cpp/test_cost.cpp SUITES unit property LIBS jarvis::kernel)
jarvis_add_layer_test(portfolio SOURCES cpp/test_portfolio.cpp SUITES unit property LIBS jarvis::kernel)
jarvis_add_layer_test(risk SOURCES cpp/test_risk.cpp SUITES unit property conformance LIBS jarvis::kernel)
target_compile_definitions(test_risk PRIVATE JARVIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
jarvis_add_layer_test(engine SOURCES cpp/test_engine.cpp SUITES unit property zero-alloc LIBS jarvis::kernel)
jarvis_add_layer_test(execution SOURCES cpp/test_execution.cpp SUITES unit property conformance LIBS jarvis::kernel)
target_compile_definitions(test_execution PRIVATE JARVIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
jarvis_add_layer_test(matching SOURCES cpp/test_matching.cpp SUITES unit property LIBS jarvis::kernel)
jarvis_add_layer_test(backtest SOURCES cpp/test_backtest.cpp SUITES unit property zero-alloc LIBS jarvis::kernel)
jarvis_add_layer_test(node SOURCES cpp/test_node.cpp SUITES unit property LIBS jarvis::shell)
target_compile_definitions(test_node PRIVATE JARVIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
jarvis_add_layer_test(node_run SOURCES cpp/test_node_run.cpp SUITES unit LIBS jarvis::shell)

# Golden trace conformance (tests/golden/<case>/case.toml). Cases run the jarvis CLI.
if(NOT TARGET jarvis_cli)
  message(FATAL_ERROR "JARVIS_BUILD_TESTS needs JARVIS_BUILD_CLI for the golden cases")
endif()
add_dependencies(jarvis_tests jarvis_cli)

# The node the golden replay cases run (tests/golden/replay_*).
add_executable(golden_node cpp/golden_node.cpp)
target_link_libraries(golden_node PRIVATE jarvis::shell)
set_target_properties(golden_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(jarvis_tests golden_node)
if(TARGET trade_logger)
  add_dependencies(jarvis_tests trade_logger)
endif()
if(TARGET pegged_mm)
  add_dependencies(jarvis_tests pegged_mm)
endif()
add_test(
  NAME golden.all
  COMMAND "${Python_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/golden.py"
          --root "${PROJECT_SOURCE_DIR}/tests/golden" --bin "${CMAKE_BINARY_DIR}"
)
set_tests_properties(golden.all PROPERTIES LABELS "golden")

# Forward trace validation (docs/architecture.md 18.2): TLC behaviours of the specs, replayed
# through the implementation. The committed files are small; the formal CI job regenerates them
# (behaviours.py --check) and replays larger fresh sets.
add_executable(trace_driver trace/trace_driver.cpp trace/behaviour_file.cpp)
target_link_libraries(trace_driver PRIVATE jarvis::kernel)
set_target_properties(trace_driver PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(jarvis_tests trace_driver)
foreach(spec IN ITEMS OrderLifecycle TradingState Matching DepthSync)
  add_test(NAME trace.${spec}
           COMMAND trace_driver "${PROJECT_SOURCE_DIR}/tests/trace/behaviours/${spec}.txt")
  set_tests_properties(trace.${spec} PROPERTIES LABELS "conformance;trace")
endforeach()

if(TARGET jarvis_network)
  jarvis_add_layer_test(network SOURCES cpp/test_network.cpp SUITES unit LIBS jarvis::network jarvis_asio)
endif()

# The fuzz targets build only with JARVIS_ENABLE_FUZZ (clang and libFuzzer). This object library,
# excluded from `all`, gives their sources real entries in compile_commands.json, so clang-tidy
# checks them with their own include paths instead of flags borrowed from a neighbouring file.
if(NOT JARVIS_ENABLE_FUZZ)
  file(GLOB _jarvis_fuzz_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/fuzz/fuzz_*.cpp")
  set(_jarvis_fuzz_libs jarvis::kernel jarvis::testkit jarvis::shell)
  if(NOT TARGET jarvis_network)
    list(FILTER _jarvis_fuzz_sources EXCLUDE REGEX "fuzz_(ws|http|json_codec)\\.cpp$")
  else()
    list(APPEND _jarvis_fuzz_libs jarvis::network jarvis::adapter)
  endif()
  add_library(jarvis_fuzz_sources OBJECT EXCLUDE_FROM_ALL ${_jarvis_fuzz_sources})
  target_link_libraries(jarvis_fuzz_sources PRIVATE ${_jarvis_fuzz_libs})
endif()

if(TARGET jarvis_adapter)
  jarvis_add_layer_test(adapter SOURCES cpp/test_binance_adapter.cpp cpp/test_depth_sync.cpp
                        cpp/test_order_tracker.cpp cpp/test_binance_requests.cpp
                        cpp/test_ws_api.cpp cpp/test_user_stream_session.cpp
                        cpp/test_startup.cpp
                        SUITES unit golden property
                        LIBS jarvis::adapter jarvis::shell jarvis::testkit jarvis_asio)
  target_compile_definitions(test_adapter PRIVATE JARVIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()

if(TARGET jarvis_live)
  jarvis_add_layer_test(live SOURCES cpp/test_live.cpp SUITES unit LIBS jarvis::live)
endif()
