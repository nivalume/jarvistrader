#!/usr/bin/env bash
# The node half of the determinism gate (docs/rust-plan.md R4, the C++ tree's
# tools/fingerprint_gate.sh): two builds of jarvis-rs (release and det-o0) write the golden catalog,
# run the probe strategy's orders plan on it with snapshots, and must produce byte-identical run
# logs and snapshots; each replays the other's run, and the fingerprint matches the golden.
#
#   tools/node_gate.sh BUILD_A BUILD_B WORK_DIR
set -euo pipefail

a="$1"
b="$2"
work="$3"
here="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$work"
mkdir -p "$work"

for side in a b; do
  bin="$a"
  [[ "$side" == b ]] && bin="$b"
  "$bin/jarvis-rs" catalog --seed 7 --out "$work/catalog-$side" --instrument > /dev/null
  "$bin/jarvis-rs" backtest --config "$here/tests/golden/node_orders/node.toml" \
    --set "data.catalog=$work/catalog-$side" --set persistence.snapshot_every=100 \
    --out "$work/run-$side" > /dev/null
done

for f in "$work"/catalog-a/BTCUSDT-PERP.BINANCE/*/2026-09-01/events-000000.jlog; do
  cmp "$f" "${f/catalog-a/catalog-b}"
done
for f in "$work"/run-a/events-*.jlog "$work"/run-a/snapshot-*.jsnap; do
  cmp "$f" "$work/run-b/$(basename "$f")"
done
# Each build replays the other's run (the run directory carries the configuration).
"$a/jarvis-rs" replay "$work/run-b" > /dev/null
"$b/jarvis-rs" replay "$work/run-a" > /dev/null
"$a/jarvis-rs" fingerprint "$work/run-a/events-000000.jlog" | diff - "$here/tests/golden/node_orders_seed7.fingerprint"
echo "node_gate: release and det-o0 runs are identical, replay each other, and match the golden fingerprint"
