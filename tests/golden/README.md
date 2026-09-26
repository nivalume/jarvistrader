# Golden traces

Each subdirectory with a `case.toml` is one golden case, run by `tools/golden.py` (ctest label
`golden`, `just golden`). A case replays an input and compares the produced files byte for byte
with the checked-in expectations. `just golden-update` rewrites the expectations and
`expected.sha256`, so every change to an expected output is visible in review.

See the module docstring of `tools/golden.py` for the `case.toml` format and
`docs/architecture.md` section 17.2 for how golden cases fit into the test harness.
