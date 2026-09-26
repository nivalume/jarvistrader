#!/usr/bin/env bash
set -euo pipefail

if [[ -n "${CLANG_TIDY:-}" ]]; then
  tool="${CLANG_TIDY}"
elif command -v clang-tidy >/dev/null 2>&1; then
  tool="$(command -v clang-tidy)"
else
  echo "clang-tidy was not found; set CLANG_TIDY or install LLVM" >&2
  exit 1
fi

if [[ ! -f build/dev/compile_commands.json ]]; then
  echo "build/dev/compile_commands.json is missing; run 'just configure dev' first" >&2
  exit 1
fi

# .clang-tidy's HeaderFilterRegex is matched against absolute paths, so a repository-relative
# pattern never matches. Anchor the filter at this checkout instead.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
header_filter="^${repo_root}/(jarvis|python|testkit|tests|benchmarks)/"

exec "${tool}" -p build/dev --header-filter="${header_filter}" "$@"
