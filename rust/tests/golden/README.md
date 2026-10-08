# Golden files of the Rust tree

- `corpus_seed7.fingerprint`: the fingerprint of the deterministic corpus for seed 7, 2000
  records, checked by `jarvis-model`'s test `seed7_corpus_fingerprint_is_golden`.
- `corpus_seed7_200000.fingerprint`: the same for 200 000 records, checked by `just rust-fp`,
  which also compares the `release` and `det-o0` builds' corpora byte for byte.

A fingerprint changes only with a deliberate change to the corpus generator or to an encoding,
which bumps `jarvis_model::event::SCHEMA_VERSION`. Regenerate with:

```sh
cargo run --release -p jarvis-cli -- corpus --seed 7 --events 200000 --out /tmp/c.jlog
cargo run --release -p jarvis-cli -- fingerprint /tmp/c.jlog > tests/golden/corpus_seed7_200000.fingerprint
```
