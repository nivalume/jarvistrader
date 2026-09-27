#!/usr/bin/env bash
# M3 acceptance (docs/plan.md): one day of BTCUSDT-PERP through the market maker examples
# against the simulated venue, in Python and in C++, with the Release and the -O0 builds.
#
#   - all four run logs are identical (the examples are twins; the builds must agree);
#   - replaying the Python run reproduces every output;
#   - the run's orders, projected on OrderLifecycle, are a behaviour of the spec (TLC);
#   - the RunReport of the run is printed.
#
#   tools/m3_acceptance.sh [DAY] [WORK_DIR]
#
# DAY defaults to 2024-03-30, the last period for which data.binance.vision publishes USDⓈ-M
# bookTicker archives. The instrument definition uses BTCUSDT's published filters (the venue's
# exchangeInfo is not reachable from every network; pass INSTRUMENT_ARGS="--exchange-info FILE"
# to use a saved response instead). The run needs about 2 GB in WORK_DIR
# (default build/acceptance-m3).
set -euo pipefail

day="${1:-2024-03-30}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(realpath -m "${2:-${root}/build/acceptance-m3}")"
python="${PYTHON:-${root}/.venv/bin/python}"
config="${root}/examples/config/mm_quote.toml"
instrument_args="${INSTRUMENT_ARGS:---tick 0.10 --step 0.001 --min-qty 0.001 --max-qty 1000 --min-notional 100 --margin-init 0.05 --margin-maint 0.004}"
mkdir -p "${work}"

cmake --build --preset rel --target _core pegged_mm jarvis_cli
cmake --build --preset det-o0 --target _core pegged_mm

package() {
  local build="$1" dir="${work}/pkg-$1"
  rm -rf "${dir}"
  mkdir -p "${dir}"
  cp -r "${root}/python/jarvis" "${dir}/"
  cp "${root}"/build/"${build}"/_core*.so "${dir}/jarvis/"
  echo "${dir}"
}
pkg_rel="$(package rel)"
pkg_o0="$(package det-o0)"

catalog="${work}/catalog"
for dataset in aggTrades bookTicker; do
  PYTHONPATH="${pkg_rel}" "${python}" -m jarvis.data binance-vision "${dataset}" --symbol BTCUSDT \
    --start "${day}" --catalog "${catalog}" --download-dir "${work}/zips" --overwrite
done
# shellcheck disable=SC2086 # instrument_args is a list of options
PYTHONPATH="${pkg_rel}" "${python}" -m jarvis.data binance-instrument --symbol BTCUSDT \
  --day "${day}" --catalog "${catalog}" ${instrument_args} --overwrite

sets=(--env backtest --set "data.catalog=${catalog}"
      --set "data.range.start=${day}T00:00:00Z" --set "data.range.end=$(date -u -d "${day} + 1 day" +%F)T00:00:00Z")
rm -rf "${work}"/run-*
PYTHONPATH="${pkg_rel}" PYTHONHASHSEED=0 "${python}" "${root}/examples/py/mm_quote.py" \
  --config "${config}" "${sets[@]}" --out "${work}/run-py-rel"
PYTHONPATH="${pkg_o0}" PYTHONHASHSEED=0 "${python}" "${root}/examples/py/mm_quote.py" \
  --config "${config}" "${sets[@]}" --out "${work}/run-py-o0" > /dev/null
"${root}/build/rel/bin/pegged_mm" --config "${config}" "${sets[@]}" --out "${work}/run-cpp-rel" > /dev/null
"${root}/build/det-o0/bin/pegged_mm" --config "${config}" "${sets[@]}" --out "${work}/run-cpp-o0" > /dev/null

status=0
for other in run-py-o0 run-cpp-rel run-cpp-o0; do
  "${root}/build/rel/bin/jarvis" fingerprint --compare "${work}/run-py-rel" "${work}/${other}" \
    --records all || status=1
done
PYTHONPATH="${pkg_rel}" PYTHONHASHSEED=0 "${python}" "${root}/examples/py/mm_quote.py" \
  --replay "${work}/run-py-rel" || status=1
"${python}" "${root}/tools/tla/check_trace.py" --spec OrderLifecycle --log "${work}/run-py-rel" \
  --jarvis "${root}/build/rel/bin/jarvis" || status=1
"${root}/build/rel/bin/jarvis" report "${work}/run-py-rel" | tee "${work}/report.txt"
if [[ ${status} -eq 0 ]]; then
  echo "m3 acceptance: PASSED"
else
  echo "m3 acceptance: FAILED"
fi
exit "${status}"
