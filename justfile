# Developer entry points (docs/architecture.md section 17.7).
# Recipes whose feature has not landed yet print "SKIPPED" and name the milestone that adds it.

set shell := ["bash", "-euo", "pipefail", "-c"]

python := ".venv/bin/python"

export PATH := justfile_directory() / ".venv/bin" + ":" + env_var("PATH")

default:
    @just --list

bootstrap:
    test -x {{python}} || uv venv --python 3.11 .venv
    uv pip install --python {{python}} "scikit-build-core==1.0.3" "pytest==9.1.1" "pre-commit==4.6.2" "actionlint-py==1.7.12.25"

configure preset="dev": bootstrap
    cmake --preset {{preset}} -DPython_EXECUTABLE="${PWD}/{{python}}"

build preset="dev": (configure preset)
    cmake --build --preset {{preset}}

install: bootstrap
    uv pip install --python {{python}} --reinstall .

# Everything a contributor runs before pushing: lint, functional tier, benchmark A/B, formal tier.
check: lint test golden fp bench-compare tla-changed

# Functional tier: every ctest label plus Python and tooling tests.
test: (build "dev") install
    ctest --preset dev
    {{python}} -m pytest

test-rel: (build "rel")
    ctest --preset rel

golden: (build "dev")
    ctest --preset dev -L golden

golden-update: (build "dev")
    {{python}} tools/golden.py --bin build/dev --update

zero-alloc: (build "dev")
    ctest --preset dev -L zero-alloc

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

# A/B benchmark comparison of the working tree against the merge-base with `base`.
bench-compare base="main" rounds="3":
    tools/bench_ab.sh {{base}} {{rounds}}

tla spec="all":
    if [[ "{{spec}}" == all ]]; then {{python}} tools/tla/run_tlc.py --all; \
    else {{python}} tools/tla/run_tlc.py --spec "{{spec}}"; fi

# Specs affected by the working tree relative to the merge-base with `base`.
tla-changed base="main":
    {{python}} tools/tla/run_tlc.py --changed --base {{base}} --worktree

trace-forward spec:
    @echo "trace-forward {{spec}}: SKIPPED - forward trace validation arrives with OrderLifecycle (plan.md M3)"

trace-backward log spec:
    @echo "trace-backward {{log}} {{spec}}: SKIPPED - 'jarvis trace-export' arrives in plan.md M3"

fuzz target="smoke" time="60":
    cmake --preset fuzz
    cmake --build --preset fuzz
    mkdir -p build/fuzz/corpus-scratch/{{target}}
    build/fuzz/tests/fuzz/fuzz_{{target}} -max_total_time={{time}} \
      build/fuzz/corpus-scratch/{{target}} tests/fuzz/corpus/{{target}}

tsan: bootstrap
    cmake --preset tsan -DPython_EXECUTABLE="${PWD}/{{python}}"
    cmake --build --preset tsan
    ctest --preset tsan

soak env="sandbox" hours="1":
    @echo "soak {{env}} {{hours}}h: SKIPPED - soak runs need the sandbox environment (plan.md M4)"

sbe-regen:
    @echo "sbe-regen: SKIPPED - the SBE codec arrives with the Spot venue (plan.md M6)"

sbe-check:
    @echo "sbe-check: SKIPPED - the SBE codec arrives with the Spot venue (plan.md M6)"

format:
    git ls-files '*.cpp' '*.hpp' '*.h' | xargs bash tools/run-clang-format.sh -i

lint: (configure "dev")
    {{python}} -m pre_commit run --all-files
