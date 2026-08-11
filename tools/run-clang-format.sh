#!/usr/bin/env bash
set -euo pipefail

if [[ -n "${CLANG_FORMAT:-}" ]]; then
  tool="${CLANG_FORMAT}"
elif command -v clang-format >/dev/null 2>&1; then
  tool="$(command -v clang-format)"
elif command -v xcrun >/dev/null 2>&1 && xcrun --find clang-format >/dev/null 2>&1; then
  tool="$(xcrun --find clang-format)"
else
  echo "clang-format was not found; set CLANG_FORMAT or install LLVM" >&2
  exit 1
fi

exec "${tool}" "$@"
