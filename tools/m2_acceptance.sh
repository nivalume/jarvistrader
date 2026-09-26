#!/usr/bin/env bash
# M2 acceptance (docs/plan.md): one day of BTCUSDT-PERP through the trade logger examples, in
# Python and in C++, with the Release and the -O0 builds. All four run logs must be identical,
# and replaying the Python run must reproduce every output.
#
#   tools/m2_acceptance.sh [DAY] [WORK_DIR]
#
# DAY defaults to 2024-03-30, the last period for which data.binance.vision publishes USDⓈ-M
# bookTicker archives. The run needs about 2 GB in WORK_DIR (default build/acceptance-m2).
set -euo pipefail

day="${1:-2024-03-30}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(realpath -m "${2:-${root}/build/acceptance-m2}")"
python="${PYTHON:-${root}/.venv/bin/python}"
config="${root}/examples/config/trade_logger.toml"
mkdir -p "${work}"

cmake --build --preset rel --target _core trade_logger jarvis_cli
cmake --build --preset det-o0 --target _core trade_logger

# A package directory per build: the Python sources with that build's extension module.
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

sets=(--env backtest --set "data.catalog=${catalog}"
      --set "data.range.start=${day}T00:00:00Z" --set "data.range.end=$(date -u -d "${day} + 1 day" +%F)T00:00:00Z")
rm -rf "${work}"/run-*
PYTHONPATH="${pkg_rel}" PYTHONHASHSEED=0 "${python}" "${root}/examples/py/trade_logger.py" \
  --config "${config}" "${sets[@]}" --out "${work}/run-py-rel"
PYTHONPATH="${pkg_o0}" PYTHONHASHSEED=0 "${python}" "${root}/examples/py/trade_logger.py" \
  --config "${config}" "${sets[@]}" --out "${work}/run-py-o0" > /dev/null
"${root}/build/rel/bin/trade_logger" --config "${config}" "${sets[@]}" --out "${work}/run-cpp-rel" > /dev/null
"${root}/build/det-o0/bin/trade_logger" --config "${config}" "${sets[@]}" --out "${work}/run-cpp-o0" > /dev/null

status=0
for other in run-py-o0 run-cpp-rel run-cpp-o0; do
  "${root}/build/rel/bin/jarvis" fingerprint --compare "${work}/run-py-rel" "${work}/${other}" \
    --records all || status=1
done
PYTHONPATH="${pkg_rel}" PYTHONHASHSEED=0 "${python}" "${root}/examples/py/trade_logger.py" \
  --replay "${work}/run-py-rel" || status=1
if [[ ${status} -eq 0 ]]; then
  echo "m2 acceptance: PASSED"
else
  echo "m2 acceptance: FAILED"
fi
exit "${status}"
