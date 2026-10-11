# Golden files of the Rust tree

- `corpus_seed7.fingerprint`: the fingerprint of the deterministic corpus for seed 7, 2000
  records, checked by `model`'s test `seed7_corpus_fingerprint_is_golden`.
- `corpus_seed7_200000.fingerprint`: the same for 200 000 records, checked by `just rust-fp`,
  which also compares the `release` and `det-o0` builds' corpora byte for byte.
- `data_seed7.fingerprint`: everything the `data` layer derives from the first 20 000 records of
  the seed 7 corpus (top of book after each update, every bar closed, every feature value),
  checked by `data`'s test `seed7_derived_data_is_golden` in the three build profiles.
- `node_orders_seed7.fingerprint` and `node_orders/node.toml`: the run log of the probe
  strategy's orders plan (`rust:Probe`, `crates/runtime/node/src/probe.rs`) on the golden catalog
  (`corpus::golden`, seed 7) against the simulated venue, with a snapshot every 100 inputs. Checked
  by `node`'s test `orders_run_fingerprint_is_golden` and by `tools/node_gate.sh` (`just rust-fp`),
  which runs it in the `release` and `det-o0` builds, compares the run logs and snapshots byte for
  byte, and has each build replay the other's run.

A corpus fingerprint changes only with a deliberate change to the corpus generator or to an
encoding (an encoding change bumps `model::event::SCHEMA_VERSION`); the data fingerprint also
changes with a deliberate change to a book, bar or feature computation; the node fingerprint with
any deliberate change to the kernel's behaviour on the orders path (the step, the risk gates, the
simulated venue, the driver's input sequence) or to the probe strategy. Regenerate with:

```sh
cargo run --release -p cli -- corpus --seed 7 --events 200000 --out /tmp/c.jlog
cargo run --release -p cli -- fingerprint /tmp/c.jlog > tests/golden/corpus_seed7_200000.fingerprint
cargo run --release -p cli -- catalog --seed 7 --out /tmp/catalog --instrument
cargo run --release -p cli -- backtest --config tests/golden/node_orders/node.toml \
  --set data.catalog=/tmp/catalog --set persistence.snapshot_every=100 --out /tmp/run
cargo run --release -p cli -- fingerprint /tmp/run/events-000000.jlog > tests/golden/node_orders_seed7.fingerprint
```
