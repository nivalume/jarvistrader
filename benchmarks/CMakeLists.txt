# Benchmarks (docs/architecture.md section 17.3). Each group is one executable:
#   bench_hot     gating benchmarks, compared A/B on every change
#   bench_report  report-only benchmarks (EngineState snapshots)
#   bench_latency report-only: the live node's tick-to-command and command-to-socket latency
# Thresholds live in benchmarks/thresholds.toml; tools/bench_compare.py applies them.

function(jarvis_add_bench_group group)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  set(target "bench_${group}")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE jarvis::strict benchmark::benchmark_main ${ARG_LIBS})
  add_dependencies(jarvis_bench ${target})
endfunction()

jarvis_add_bench_group(
  hot SOURCES hot/bench_noop.cpp hot/bench_model.cpp hot/bench_engine.cpp hot/bench_execution.cpp
  LIBS jarvis::kernel
)

jarvis_add_bench_group(report SOURCES report/bench_snapshot.cpp LIBS jarvis::kernel)

# The venue codecs need the live shell (JARVIS_BUILD_LIVE); a separate gating group so the kernel
# group builds without it.
if(TARGET jarvis_adapter)
  jarvis_add_bench_group(codec SOURCES hot/bench_codec.cpp LIBS jarvis::adapter)
endif()

# The live rings: header-only, but they belong to the live shell and cross threads.
if(TARGET jarvis_live)
  jarvis_add_bench_group(live SOURCES hot/bench_ring.cpp LIBS jarvis::live)
endif()

# The live node's latency against a local venue over loopback TLS (report only): uses the mock
# servers of tests/cpp/support.
if(TARGET jarvis_live)
  jarvis_add_bench_group(latency SOURCES report/bench_latency.cpp LIBS jarvis::live jarvis_asio)
  target_include_directories(bench_latency PRIVATE "${PROJECT_SOURCE_DIR}/tests/cpp")
  target_compile_definitions(bench_latency PRIVATE JARVIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(bench_latency PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
