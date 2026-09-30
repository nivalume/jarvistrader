#!/usr/bin/env bash
# Determinism gate (docs/architecture.md section 17.3): two builds of the same commit, typically
# Release and -O0, generate the same deterministic corpus; the event logs must be byte-identical.
# The corpus runs every event kind through the model's parsing, formatting and fixed-point
# arithmetic, which is where optimisation-dependent behaviour would show up. From M2 on, golden
# replays through the engine join this gate, and with M5 the EngineState snapshots.
#
#   tools/fingerprint_gate.sh <build-dir-a> <build-dir-b> [seed] [events]
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <build-dir-a> <build-dir-b> [seed] [events]" >&2
  exit 2
fi
a="$1"
b="$2"
seed="${3:-7}"
events="${4:-200000}"
out="build/fp"

rm -rf "$out"
mkdir -p "$out"
"$a/bin/jarvis" corpus --seed "$seed" --events "$events" --out "$out/a"
"$b/bin/jarvis" corpus --seed "$seed" --events "$events" --out "$out/b"
echo "fp: $a vs $b, seed $seed, $events events"
"$a/bin/jarvis" fingerprint --records all --compare "$out/a" "$out/b"

# EngineState snapshots (section 16.3) through the engine: the golden orders plan (simulated
# venue, fills, cancels, modifies) with a snapshot every 100 inputs. The run logs must match, and
# so must every snapshot's state bytes (the files' headers name the build, so the state is
# compared through its digest).
if [[ -x "$a/bin/golden_node" && -x "$b/bin/golden_node" ]]; then
  for side in a b; do
    bin="$a"
    [[ "$side" == b ]] && bin="$b"
    "$bin/bin/golden_node" catalog --seed 7 --out "$out/catalog-$side" --instrument > /dev/null
    "$bin/bin/golden_node" --config tests/golden/replay_orders/node.toml \
      --set "data.catalog=$out/catalog-$side" --set persistence.snapshot_every=100 \
      --out "$out/orders-$side" > /dev/null
  done
  "$a/bin/jarvis" fingerprint --records all --compare "$out/orders-a" "$out/orders-b"
  count=0
  for f in "$out"/orders-a/snapshot-*.jsnap; do
    name="$(basename "$f")"
    sa="$("$a/bin/jarvis" snapshot "$f" | grep '^state ')"
    sb="$("$b/bin/jarvis" snapshot "$out/orders-b/$name" | grep '^state ')"
    if [[ "$sa" != "$sb" ]]; then
      echo "snapshot $name differs: $sa vs $sb" >&2
      exit 1
    fi
    count=$((count + 1))
  done
  if [[ "$count" -eq 0 ]]; then
    echo "no snapshots were taken" >&2
    exit 1
  fi
  echo "identical: $count snapshots"
fi
