# libFuzzer targets (docs/architecture.md section 17.2). Built only with JARVIS_ENABLE_FUZZ,
# which instruments first-party code; each target links the libFuzzer driver.
# Seed corpora live in tests/fuzz/corpus/<target>/ and are read-only inputs; `just fuzz`
# writes discovered inputs to a scratch directory under the build tree.

function(jarvis_add_fuzz_target name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  set(target "fuzz_${name}")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE jarvis::kernel ${ARG_LIBS})
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  add_dependencies(jarvis_fuzz ${target})
endfunction()

# Pipeline check: exercises the testkit generator so the fuzz build, driver and corpus handling
# are proven before real decoders exist.
jarvis_add_fuzz_target(smoke SOURCES fuzz_smoke.cpp LIBS jarvis::testkit)
jarvis_add_fuzz_target(decimal SOURCES fuzz_decimal.cpp)
jarvis_add_fuzz_target(wire SOURCES fuzz_wire.cpp)
jarvis_add_fuzz_target(config SOURCES fuzz_config.cpp LIBS jarvis::shell)
if(TARGET jarvis_network)
  jarvis_add_fuzz_target(ws SOURCES fuzz_ws.cpp LIBS jarvis::network)
  jarvis_add_fuzz_target(http SOURCES fuzz_http.cpp LIBS jarvis::network)
endif()
