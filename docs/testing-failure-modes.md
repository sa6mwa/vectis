# Fast failure-mode feedback

Run `make test-failures-fast` for local failure-mode checks. It incrementally
builds the Vectis CLI and the native lifetime/supervisor/audio units selected
by the `failure-fast` CTest label, then runs that label serially.
`make test` runs this gate before the remaining debug tests, so a failure is
reported before the longer existing suites. These tests are also registered
normally for direct CTest and release gates.

Failure injection uses deterministic allocator faults, callbacks and process/thread
handshakes rather than sleeps or deliberate timeout workloads. Native tests use
local threads/forks; real normal/packed fixtures use loopback and generated
credentials/storage, not external services or containers. HTTP harnesses wait
on readiness/effect output with selectors. Deadlines bound unexpected hangs;
recovery/timer fixtures exercise scheduling without adding fixed test delays.
Generated fixtures and packed executables stay under the build directory.

The libmdf 0.14.0 upgrade candidate passed **37** fast checks, including two new
rendering checks, followed by 176 broader tests. The new C test takes under
0.01s and normal/packed Lua regression about 0.2s locally.

The earlier integration candidate `e20ab55` passed all **35** fast tests in **16.05s**,
excluding the build, followed by 176 broader checks. The earlier encoder/MCP-only
23-test measurement was superseded for that candidate. Its construction
and cleanup matrix still supplies 1,456 assertions; its HTTP matrix adds 96
assertions and two uncaught-error exits. Timings are local observations, not
portable runtime guarantees. See the [seam audit](lua-upgrade-seam-audit.md#completed-independent-review-and-final-verification)
for exact commands and the separate nonclean full-service Memcheck diagnostic.

## Coverage

- libmdf fixed emission buffers report `MDF_ERROR_NOMEM` with no output; normal
  and packed Lua exercise fragmented hyperlink wrapping, escaped table pipes,
  font deduplication, manual finish, reader/writer failure and output before EOF.
- All 16 combinations of access logging and Lockd, CAI, and HTTP subsystem
  logging, with invalid constructor options and native configuration failures.
- Rejected MCP registration before and after callback allocation, after partial
  tool registration, with unsupported sessions or methods, and at native route
  validation. Rejection must leave route counts unchanged, release callbacks,
  and allow a subsequent valid registration.
- Repeated close, garbage collection, and scope unwinding, with weak references
  proving app and callback owners are released. Live callbacks must survive
  collection before close.
- Real MCP execution in direct, supervised, and packed supervised applications:
  throwing callbacks, nil results, explicit callback rejection, non-string
  exceptions, incorrect result types, malformed JSON results, output limits,
  missing tools, invalid arguments, malformed requests, unknown methods,
  oversized complete and unfinished request bodies, and untrusted origins.
  A successful call after each rejection proves continued service.
- Uncaught top-level errors with a live app in script and packed execution must
  exit with the ordinary error status, rather than crashing during finalization.
- Both bundled lonejson encoder aliases at plain and escaped Unicode string
  boundaries and full pipeline records at 1, 10, 50, 100, 200, 400, and 600 rows.
  Successful encoding must round-trip every field.

- Actual worker commit receipts must wake the owner dispatcher with durable
  discovery disabled and a candidate probe interval beyond the test deadline;
  separate saturation cases prove repair, not direct-path latency.
- Bounded IPC full/closed/oversize cases, coalesced wake races, owner process/thread
  restrictions, retained mappings, timer disable and scope lifecycle errors.
- Unknown workflow kinds, rollback-only transactions, explicit outcome lifetimes,
  queue failure accounting and canonical client option propagation.
- Throwing response/source getters, read/reset failures, close/reentry, coroutine
  ownership, allocation failures after callback return, exact-once cleanup and
  healthy continuation across normal and packed execution.
- OPC UA logger replacement/ownership and SUS foreign-thread log delivery,
  retained callback collection and managed-service teardown.

## Encoder regression

The encoder boundary test intentionally asserts the required successful behavior.
It has no expected-failure flag or suppression. Lonejson 0.44.0 failed 22 of
its 38 checks. Upgrading to lonejson 0.45.0 fixes all of them and restores the
ordinary gates without changing the test. Future failures identify the affected
alias, boundary, and fixture size. Keep this regression in the fast and standard
gates. The original upstream report is retained as a separate self-contained
Markdown deliverable under `stash/`.
