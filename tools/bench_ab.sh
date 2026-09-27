#!/usr/bin/env bash
# A/B benchmark run on one machine (docs/architecture.md section 17.3).
#
#   tools/bench_ab.sh [BASE_REF] [ROUNDS]
#
# Builds the merge-base of BASE_REF and HEAD in a git worktree, builds the current working tree
# (uncommitted changes included), then runs every benchmark group of both builds alternately for
# ROUNDS rounds, pinned to one CPU when taskset exists. Results go to build/ab/, and
# tools/bench_compare.py decides whether a gating regression reproduced.
#
# Environment: BENCH_REPETITIONS (default 10), BENCH_MIN_TIME (default 0.5s),
# BENCH_AB_DIR (default build/ab), CPM_SOURCE_CACHE (default build/cpm-cache).
set -euo pipefail

base_ref="${1:-main}"
rounds="${2:-3}"
repetitions="${BENCH_REPETITIONS:-10}"
min_time="${BENCH_MIN_TIME:-0.5s}"

root="$(git rev-parse --show-toplevel)"
work="${BENCH_AB_DIR:-${root}/build/ab}"
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-${root}/build/cpm-cache}"

base_commit="$(git -C "${root}" merge-base "${base_ref}" HEAD)"
base_src="${work}/base-src"
mkdir -p "${work}"
rm -f "${work}"/r*-*.json "${work}/summary.md" "${work}/bench.log"

cleanup() {
  git -C "${root}" worktree remove --force "${base_src}" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup
git -C "${root}" worktree add --detach --quiet "${base_src}" "${base_commit}"

configure_and_build() {
  local src="$1" build="$2"
  cmake -S "${src}" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DJARVIS_BUILD_PYTHON=OFF -DJARVIS_BUILD_TESTS=OFF -DJARVIS_BUILD_BENCHMARKS=ON \
    -DJARVIS_BUILD_LIVE=ON >/dev/null
  cmake --build "${build}" --target jarvis_bench >/dev/null
}

echo "bench-ab: base ${base_commit} (merge-base of ${base_ref} and HEAD), head = working tree"
configure_and_build "${base_src}" "${work}/base-build"
configure_and_build "${root}" "${work}/head-build"

mapfile -t groups < <(cd "${work}/head-build" && find benchmarks -maxdepth 1 -type f -name 'bench_*' -perm -u+x \
  -exec basename {} \; 2>/dev/null | sort)
if [[ ! -d "${work}/base-build/benchmarks" ]]; then
  echo "bench-ab: SKIPPED: base ${base_commit} has no benchmarks to compare against"
  exit 0
fi

pin=()
if command -v taskset >/dev/null 2>&1; then
  pin=(taskset -c "$(($(nproc) - 1))")
fi

run_group() {
  local side="$1" group="$2" round="$3"
  "${pin[@]}" "${work}/${side}-build/benchmarks/${group}" \
    --benchmark_repetitions="${repetitions}" --benchmark_min_time="${min_time}" \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="${work}/r${round}-${side}-${group}.json" --benchmark_out_format=json \
    >/dev/null 2>>"${work}/bench.log"
}

pairs=()
for group in "${groups[@]}"; do
  if [[ ! -x "${work}/base-build/benchmarks/${group}" ]]; then
    echo "bench-ab: ${group} is new in head; reported without comparison"
    continue
  fi
  for ((round = 1; round <= rounds; round++)); do
    if ((round % 2 == 1)); then
      run_group base "${group}" "${round}"
      run_group head "${group}" "${round}"
    else
      run_group head "${group}" "${round}"
      run_group base "${group}" "${round}"
    fi
    pairs+=("${work}/r${round}-base-${group}.json" "${work}/r${round}-head-${group}.json")
  done
done

if ((${#pairs[@]} == 0)); then
  echo "bench-ab: SKIPPED: no benchmark group exists in both base and head"
  exit 0
fi

python3 "${root}/tools/bench_compare.py" --thresholds "${root}/benchmarks/thresholds.toml" \
  --summary "${work}/summary.md" "${pairs[@]}"
