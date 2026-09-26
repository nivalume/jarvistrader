# Benchmarks (docs/architecture.md section 17.3). Each group is one executable:
#   bench_hot     gating benchmarks, compared A/B on every change
#   bench_report  report-only benchmarks (added when the first one exists)
# Thresholds live in benchmarks/thresholds.toml; tools/bench_compare.py applies them.

function(jarvis_add_bench_group group)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  set(target "bench_${group}")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE jarvis::strict benchmark::benchmark_main ${ARG_LIBS})
  add_dependencies(jarvis_bench ${target})
endfunction()

jarvis_add_bench_group(hot SOURCES hot/bench_noop.cpp hot/bench_model.cpp LIBS jarvis::kernel)
