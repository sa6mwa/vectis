# Fast failure-mode feedback

Run `make test-failures-fast` for local failure-mode checks. It incrementally
builds only the Vectis CLI and runs the `failure-fast` CTest label serially.
`make test` runs this gate before the remaining debug tests, so a failure is
reported before the longer existing suites. These tests are also registered
normally for direct CTest and release gates.

The tests use no sleeps, polling loops, remote services, credentials, containers,
or deliberate timeout experiments. The HTTP harness waits on startup output
using a selector, then sends requests over loopback. Its readiness, request, and
shutdown deadlines only bound unexpected hangs; they are not timed workloads.
Generated fixtures and packed executables stay under the build directory.

On the upgraded native debug build, all 23 tests passed in 2.30 seconds excluding
the build. The construction and cleanup matrix counts 1,456 checks; the HTTP
matrix adds 96 checks and two uncaught-error exit checks. These are measured
local timings, not a portable runtime guarantee.

## Coverage

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

## Encoder regression

The encoder boundary test intentionally asserts the required successful behavior.
It has no expected-failure flag or suppression. Lonejson 0.44.0 failed 22 of
its 38 checks. Upgrading to lonejson 0.45.0 fixes all of them and restores the
ordinary gates without changing the test. Future failures identify the affected
alias, boundary, and fixture size. Keep this regression in the fast and standard
gates. The original upstream report is retained as a separate self-contained
Markdown deliverable under `stash/`.
