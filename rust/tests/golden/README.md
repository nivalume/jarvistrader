# Golden files of the Rust tree

- `corpus_seed7.fingerprint`: the fingerprint of the deterministic corpus for seed 7, 2000
  records, checked by `model`'s test `seed7_corpus_fingerprint_is_golden`.
- `corpus_seed7_200000.fingerprint`: the same for 200 000 records, checked by `just rust-fp`,
  which also compares the `release` and `det-o0` builds' corpora byte for byte.
- `data_seed7.fingerprint`: everything the `data` layer derives from the first 20 000 records of
  the seed 7 corpus (top of book after each update, every bar closed, every feature value),
  checked by `data`'s test `seed7_derived_data_is_golden` in the three build profiles.

A corpus fingerprint changes only with a deliberate change to the corpus generator or to an
encoding (an encoding change bumps `model::event::SCHEMA_VERSION`); the data fingerprint also
changes with a deliberate change to a book, bar or feature computation. Regenerate with:

```sh
cargo run --release -p cli -- corpus --seed 7 --events 200000 --out /tmp/c.jlog
cargo run --release -p cli -- fingerprint /tmp/c.jlog > tests/golden/corpus_seed7_200000.fingerprint
```
