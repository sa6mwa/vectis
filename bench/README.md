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

The CMake-declared native fixture uses the existing host Go toolchain, its
standard library and the same Lua workload. Lists in proxy benchmark arguments
use comma-separated values, for example `--concurrency 1,8,16`.
No Python execution or assertion controller is involved. The full run
processes every minute of both histories and can be very expensive, particularly
for whole-partition eager rewriting. It reports actual elapsed write/read times,
operation counts and payload bytes; these are single runs, not statistical
confidence intervals. Repeat full runs before making a production decision.

The smoke gate uses 1501 source samples, three-slot retention rings and two-slot
shards to exercise rollover, all aggregation boundaries and partial final buckets.
Its timing must not be used to choose a production layout.
It is registered with CTest when `VECTIS_BUILD_BENCHMARKS=ON`.

Generated encrypted roots and their keys are created strictly under `build/` and
removed after each successful scenario. Failed runs retain their owned evidence
directory and report its path. Neither
normal metrics state nor authentication state is accessed. No transactional
outbox is involved.

`make perf-gate` checks actual encrypted-metrics recovery through HTTPS, with
20,000 updates, two clean restarts, shared-writer crash recovery and rejection
of an unexpired exclusive writer. The crash barrier is an observed `SEEDED`
event followed by SIGKILL; it needs no sleep or expiry wait. Startup uses the
app-start acknowledgement and a certificate-verified HTTPS response. The gate
also bounds process-tree CPU, including shutdown cleanup.

Sanitizer/coverage configurations run the same full recovery workload as
`vectis_metrics_recovery_instrumented`. They retain readiness deadlines, restored
counts, crash/lease behavior and cleanup checks, but use `--cpu-seconds 0` to
measure CPU without applying the normal build's numerical performance budget.
The ordinary `vectis_metrics_recovery_performance` gate retains its two-second
CPU bound. Instrumented success is functional/memory evidence, not performance
approval.

CTest, `perf-gate` and service e2e share one native CMake producer for this
expensive suite. Its success stamp depends on the actual CLI, compiled fixture
and Lua input; failure removes the stamp. Full clean removes all results.
To force a fresh run, remove `build/debug/test-results/metrics-recovery.passed`
before invoking the gate. Changed environment or restored/copied timestamps
require explicit invalidation under the lifecycle's stable-workspace contract.
`vectis_metrics_recovery_fast` uses 16 updates for early failure feedback and
does not establish the full workload's performance bound.
