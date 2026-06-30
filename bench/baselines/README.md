# liblql Benchmark Baselines

This directory contains committed benchmark baseline logs produced by:

```sh
make bench-freeze-baseline
```

The freeze target waits for a quiet host load, runs the deterministic benchmark
gates with the optimized C benchmark helper, validates the emitted JSON Lines
records, and replaces the baseline logs plus `SHA256SUMS`.

These logs are C-native performance baselines for this repository. Go timings
remain behavioral context, not the target performance level.
