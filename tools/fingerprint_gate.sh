#!/usr/bin/env bash
# Determinism gate (docs/architecture.md section 17.3): two builds of the same commit, typically
# Release and -O0, generate the same deterministic corpus; the event logs must be byte-identical.
# The corpus runs every event kind through the model's parsing, formatting and fixed-point
# arithmetic, which is where optimisation-dependent behaviour would show up. From M2 on, golden
# replays through the engine join this gate.
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
