# Developer entry points (docs/architecture.md section 17.7).
# Recipes whose feature has not landed yet print "SKIPPED" and name the milestone that adds it.

set shell := ["bash", "-euo", "pipefail", "-c"]
# Windows: the bash of Git for Windows (on PATH).
set windows-shell := ["bash", "-euo", "pipefail", "-c"]

windows := if os_family() == "windows" { "true" } else { "false" }
venv_bin := if windows == "true" { ".venv/Scripts" } else { ".venv/bin" }
python := if windows == "true" { venv_bin / "python.exe" } else { venv_bin / "python" }
path_separator := if windows == "true" { ";" } else { ":" }
# The everyday build: Debug with ASan and UBSan; on Windows, Debug with clang-cl (no sanitizers).
dev := if windows == "true" { "win-dev" } else { "dev" }

export PATH := justfile_directory() / venv_bin + path_separator + env_var("PATH")

default:
    @just --list

bootstrap:
    test -x {{python}} || uv venv --python 3.11 .venv
    uv pip install --python {{python}} "scikit-build-core==1.0.3" "pytest==9.1.1" "pre-commit==4.6.2" "actionlint-py==1.7.12.25"

configure preset=dev: bootstrap
    cmake --preset {{preset}} -DPython_EXECUTABLE="{{justfile_directory() / python}}"

build preset=dev: (configure preset)
    cmake --build --preset {{preset}}

install: bootstrap
    uv pip install --python {{python}} --reinstall ".[parquet]"

# The same with the live shell (needs OpenSSL 3 headers): jarvis.Node then runs sandbox sessions.
install-live: bootstrap
    uv pip install --python {{python}} --reinstall ".[parquet]" -C cmake.define.JARVIS_BUILD_LIVE=ON

# Python files load from python/jarvis; after a C++ change the extension rebuilds itself on
# import (cmake and ninja on PATH). `just install` and `just test` replace it with a regular one.
# Editable install with the live shell, for daily work (docs/development.md).
develop: bootstrap
    uv pip install --python {{python}} --no-build-isolation -e ".[parquet]" \
      -C cmake.define.JARVIS_BUILD_LIVE=ON -C editable.rebuild=true -C editable.verbose=false

# Everything a contributor runs before pushing: lint, functional tier, benchmark A/B, formal tier.
check: lint test golden fp bench-compare tla-changed

# Functional tier: every ctest label plus Python and tooling tests.
test: (build dev) install
    ctest --preset {{dev}}
    {{python}} -m pytest

test-rel: (build "rel")
    ctest --preset rel

golden: (build dev)
    ctest --preset {{dev}} -L golden

golden-update: (build dev)
    {{python}} tools/golden.py --bin build/{{dev}} --update

zero-alloc: (build dev)
    ctest --preset {{dev}} -L zero-alloc

layering:
    {{python}} tools/check-layering.py

# Determinism gate: Release and -O0 builds must write byte-identical event logs.
fp seed="7" events="200000": (build "rel") (build "det-o0")
    tools/fingerprint_gate.sh build/rel build/det-o0 {{seed}} {{events}}

bench: (build "bench")
    mkdir -p benchmark-results
    for exe in build/bench/benchmarks/bench_*; do \
      "$exe" --benchmark_out="benchmark-results/$(basename "$exe").json" --benchmark_out_format=json; \
    done

# Report-only Python callback benchmark (docs/architecture.md 7.8); needs `just install` first.
bench-py:
    mkdir -p benchmark-results
    {{python}} benchmarks/report/bench_py_callback.py --out benchmark-results/bench_py.json

# A/B benchmark comparison of the working tree against the merge-base with `base`.
bench-compare base="main" rounds="3":
    tools/bench_ab.sh {{base}} {{rounds}}

tla spec="all":
    if [[ "{{spec}}" == all ]]; then {{python}} tools/tla/run_tlc.py --all; \
    else {{python}} tools/tla/run_tlc.py --spec "{{spec}}"; fi

# Specs affected by the working tree relative to the merge-base with `base`.
tla-changed base="main":
    {{python}} tools/tla/run_tlc.py --changed --base {{base}} --worktree

# Forward trace validation (docs/architecture.md 18.2): fresh TLC behaviours through the code,
# after checking that the committed set ctest replays is current.
trace-forward spec num="2000" depth="24" seed="1": (build dev)
    {{python}} tools/tla/behaviours.py --check tests/trace/behaviours/{{spec}}.txt
    {{python}} tools/tla/behaviours.py --spec {{spec}} --num {{num}} --depth {{depth}} --seed {{seed}}
    build/{{dev}}/bin/trace_driver build/tla/behaviours/{{spec}}.txt

# Backward trace validation: an event log (or run directory) projected on the spec, checked by TLC.
trace-backward log spec="OrderLifecycle": (build dev)
    {{python}} tools/tla/check_trace.py --jarvis build/{{dev}}/bin/jarvis --spec {{spec}} --log {{log}}

fuzz target="smoke" time="60":
    cmake --preset fuzz
    cmake --build --preset fuzz
    mkdir -p build/fuzz/corpus-scratch/{{target}}
    build/fuzz/tests/fuzz/fuzz_{{target}} -max_total_time={{time}} \
      build/fuzz/corpus-scratch/{{target}} tests/fuzz/corpus/{{target}}

tsan: bootstrap
    cmake --preset tsan -DPython_EXECUTABLE="{{justfile_directory() / python}}"
    cmake --build --preset tsan
    ctest --preset tsan

# The live node against a faulty venue (tests/cpp/test_chaos.cpp), `seeds` seeds.
chaos seeds="5": (build dev)
    JARVIS_CHAOS_SEEDS={{seeds}} build/{{dev}}/tests/test_chaos

# One seed under ASan and UBSan, its run kept in build/soak; TLC needs Java.
# The nightly soak: the chaos test for `seconds`, then backward trace validation of its log.
soak seconds="600": (build dev)
    rm -rf build/soak && mkdir -p build/soak
    JARVIS_CHAOS_SEEDS=1 JARVIS_CHAOS_SOAK_S={{seconds}} JARVIS_CHAOS_KEEP=build/soak \
      build/{{dev}}/tests/test_chaos
    {{python}} tools/tla/check_trace.py --spec OrderLifecycle --log build/soak/seed-1 \
      --jarvis build/{{dev}}/bin/jarvis

sbe-regen:
    @echo "sbe-regen: SKIPPED - the SBE codec arrives with the Spot venue (plan.md M6)"

sbe-check:
    @echo "sbe-check: SKIPPED - the SBE codec arrives with the Spot venue (plan.md M6)"

format:
    git ls-files '*.cpp' '*.hpp' '*.h' | xargs bash tools/run-clang-format.sh -i

lint: (configure dev)
    {{python}} -m pre_commit run --all-files
