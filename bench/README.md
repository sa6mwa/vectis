# Metrics Storage Benchmark

This opt-in benchmark measures real encrypted Pouch updates through Vectis's
Lua lockdc binding. It does not change production metrics storage or Smith.

Each scenario receives the same deterministic one-minute source sample stream
for 180 and 365 days. A sample contains eight low-cardinality metric values.
Each source sample is aggregated incrementally into these retention rings:

- One-minute samples: 24 hours.
- Five-minute aggregates: 30 days.
- Hourly aggregates: 90 days.
- Daily aggregates: the 180- or 365-day horizon only.

All scenarios retain the same samples and eight metric sums plus sample count
per record. Timestamps distinguish ring generations; old slots are overwritten.
No scenario keeps 180/365 days of minute-resolution data.

The layouts are one JSON document per sample, one document containing the entire
retention partition, and documents sharded into groups of 64 samples. Every record
contains all metrics; there is no key per individual metric. Each layout is tested
with boundary-only writes and eager writes of all four current aggregates every
minute. A final partial bucket is flushed in both schedules.

Updates perform actual acquire/read/decode/modify/encode/update/release operations;
they do not reconstruct past JSON from deterministic input to bypass parsing.
JSON documents are explicitly buffered by lockdc's JSON helpers. This is not a
streaming benchmark. Keys live in `vectis.metrics.bench` in fresh isolated roots.
The write timer includes closing/flushing the client. The read timer includes
reopening the same encrypted root, not merely reading the writer's live state.
Readback validates every retained timestamp, sample count and aggregate value,
then compares retained counts and checksums across all scenarios.

Run it through the repository command surface:

```sh
make bench-metrics-storage

# Short correctness gate, exercising ring wraparound and partial final buckets:
make bench-metrics-storage METRICS_BENCH_ARGS=--smoke
```

Python 3.9+ is required for orchestration and wall-clock timing. The full run
processes every minute of both histories and can be very expensive, particularly
for whole-partition eager rewriting. It reports actual elapsed write/read times,
operation counts and payload bytes; these are single runs, not statistical
confidence intervals. Repeat full runs before making a production decision.

The smoke gate uses 1501 source samples, three-slot retention rings and two-slot
shards to exercise rollover, all aggregation boundaries and partial final buckets.
Its timing must not be used to choose a production layout.
It is registered with CTest when `VECTIS_BUILD_BENCHMARKS=ON`.

Generated encrypted roots and their keys are created strictly under `build/` and
removed after each scenario. Cleanup also runs when a child fails. Neither
normal metrics state nor authentication state is accessed. No transactional
outbox is involved.
