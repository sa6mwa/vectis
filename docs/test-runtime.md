# Pinned Bootlin runtime for local executables

Local GNU/Linux x86-64 executables use the configured Bootlin interpreter and
runtime libraries directly. No Bubblewrap, container, CTest launcher, or duplicate
test CLI is involved. The current collection is stable-2026.08-1 (glibc 2.44).

## Artifact boundary

| Artifact | Runtime |
| --- | --- |
| Debug CLI, tests, compiled helpers, examples, fuzzers, benchmarks | Pinned Bootlin interpreter and runtime search paths |
| Linux Release CLI | Statically linked; normal distributable artifact |
| Shipped libraries and SDK metadata | No local-executable runtime settings |
| Python, CMake, shell, Valgrind | Host runtime |

`cmake/test_runtime.cmake` applies private executable link settings after target
creation. DT_RPATH covers transitive dependencies as well as direct dependencies.
The configured sysroot is the source of paths, not a duplicated cache location.
Library targets and their exported usage requirements are not modified.

```sh
make test
ctest --preset debug -R vectis_runtime_contract --output-on-failure
build/debug/vectis --version
make run-example EXAMPLE=mdf_render
```

ASan, coverage and fuzz presets use the same target policy. ASan/UBSan executable
runtimes are linked statically from Bootlin: the collection's sanitizer DSOs
have their own `$ORIGIN` RUNPATH, which otherwise allowed transitive host `libm`
resolution in a helper. The executable still uses Bootlin's dynamic libc and
loader. Cross-architecture and Darwin execution are not covered by this native
runner policy.

Each child executable selects its own interpreter. In-tree examples and helpers
therefore need no special invocation. Direct execution also preserves the
application's `/proc/self/exe`, unlike explicitly invoking the ELF loader.
Executables packed using debug Vectis remain local development artifacts;
production packing must use the static release CLI.

## Verification and limits

The runtime contract enumerates configured local executable targets, checks their
ELF interpreters, and traces resolved libraries with inherited `LD_*` settings
removed. Toolchain-provided dependencies must resolve to the pinned files;
unexpected host dependencies fail the gate. It also checks child self-exec and
includes a missing-runtime negative fixture. A Release run additionally checks
that the CLI has no interpreter or dynamic dependencies.

Target enumeration uses one regenerated manifest, so disabling a target cannot
leave a stale entry in the gate. A reconfiguration regression builds and runs a
helper after removing an optional target. The standalone fuzz smoke entry point
also runs the mapping gate before AFL, without relying on CTest.

This is runtime selection, not isolation. Missing runtime files can otherwise
fall back through the system loader cache; even `-z nodefaultlib` does not exclude
every host multiarch cache entry. The contract detects foreign mappings rather
than trusting a linker flag. Direct execution outside the test environment can
also be influenced by user-supplied loader variables. Do not set a global
Bootlin `LD_LIBRARY_PATH`, which would affect host tools too.

CTest clears loader overrides and places default XDG state and temporary files
under the configured build directory. Shared dependency/toolchain cache locations
are preserved. This fixture isolation is separate from runtime selection.

SDK verification builds CMake and pkg-config consumers with the pinned toolchain
and gives those local consumers Bootlin runtime paths. Installed example sources
remain ordinary consumer sources. Host compatibility is a separate concern and
is not established by the pinned-runtime test suite.
The mapping gate covers pkg-config executables as well as CMake consumers and
installed examples. Native GNU release-matrix SDK smoke builds use the same
private link policy; cross-target smoke builds remain compile/link-only.
Linux SDK and static-release smoke checks do not inject `LD_LIBRARY_PATH`.

Release packaging rejects non-Release builds. Linux development CLI targets are
not installed; library SDK development installs remain available. Existing
release privacy checks remain responsible for final archive metadata.

## Measurement method

Measure execution, excluding the build, in a fresh cgroup for each run:

```sh
systemd-run --user --wait --pipe --property=MemoryAccounting=yes \
  --working-directory="$PWD" --unit=vectis-runtime-measure-1 \
  python3 scripts/measure_test_runtime.py ctest --preset debug
```

Use a new unit name for each run. systemd/cgroup v2 is a measurement dependency,
not a test execution dependency. The script records monotonic wall time, total
process-tree user/system CPU, aggregate cgroup memory peak and swap peak. Memory
includes charged file cache and the small measurement process; it is not the
largest individual process's RSS. Results describe this workload and machine,
not a universal fixed overhead.

Baseline logs are retained under `build/runtime-before-*.log`; the direct-link
results are under `build/runtime-direct-*.log`. Earlier Bubblewrap experiments
are superseded and must not be reported as the direct-link implementation's cost.

## Full-suite measurements

Measured on Linux 7.0.0-30-generic, four virtual AMD EPYC-Rome-v5 CPUs, CTest
4.2.3. The host baseline used glibc 2.43; the selected Bootlin runtime is 2.44.
All runs were serial, without concurrent builds or test suites.
The measured debug configuration had `VECTIS_BUILD_BENCHMARKS=ON`, including the
metrics-storage smoke test; a fresh default debug configuration has one fewer
test. The ASan configuration uses the default benchmark setting (off).

| Run | Tests passed | Wall seconds | User CPU seconds | System CPU seconds | Aggregate peak MiB |
| --- | --- | ---: | ---: | ---: | ---: |
| Host baseline 1 | 88/88 | 105.616198 | 24.766719 | 24.853652 | 1074.035156 |
| Host baseline 2 | 88/88 | 103.934081 | 24.931438 | 22.791359 | 1073.738281 |
| Direct Bootlin 1 | 89/89 | 110.939282 | 24.501646 | 25.282166 | 1072.859375 |
| Direct Bootlin 2 | 89/89 | 107.031196 | 24.347970 | 23.371248 | 1072.699219 |

Mean changes: wall time +4.210099 seconds (+4.018%); total CPU +0.079932
CPU-seconds (+0.164%); aggregate peak memory -1.107422 MiB (-0.103%). All swap
peaks were zero. These are observed differences, not statistically established
fixed overheads.

The second direct run used `make -j1 test`, overriding only `CTEST` with the
measurement command above; its prerequisite build ran outside the measured
cgroup. The first used ordinary `ctest --preset debug` after building.

Comparability limits: the new suite adds the runtime contract, isolates default
CTest state under build/, resets a persistent auth fixture, and relaxes SSH
fixture handshake budgets. Two deliberate SSH timeout cases now use 2 seconds
rather than 200 ms (about 3.6 seconds of expected added wall time); ordinary SFTP
handshakes get 8 seconds rather than 1 second. The tests still assert failure
context and bounded completion. This prevents handshake failures from masking
the intended output/completion checks. Consequently, the +4.21 seconds cannot
be attributed solely to selecting Bootlin's libc.

## Verification record

- Two complete measured debug suites: 89/89 each; the second through `make test`.
  A final unmeasured `make test` run also passed 89/89.
- Service e2e passed, including metrics recovery, packed applications, TLS/ACME,
  SSH/SFTP, Lockd services and authenticated WebDAV examples.
- Static and shared SDK consumers and their 36 installed examples each passed
  pinned-runtime checks. The shared SDK install also confirmed no debug CLI was
  installed.
- Linux Release CLI built and ran; a release-packed Markdown example ran.
  ELF inspection found no interpreter/dynamic dependency section in these
  release executables. This was a release CLI/packing smoke check, not a full
  release runtime suite or a newly published release matrix.
- Formatting and lifecycle/privacy contract checks passed.
- All five configured Valgrind checks passed with zero reported errors.
- Sanitizer runtime-path verification passed after linking the pinned sanitizer
  runtimes statically, including a generated instrumented shared-library consumer
  and child self-exec. The final full sanitizer run passed 87/88; the SSH finding
  below is the only remaining failure.

### Remaining sanitizer finding

The SSH output fixture reproducibly reports two 48-byte leaks with allocation
stacks in `kex_method_curve25519_key_exchange` (libssh2), in its `stderr` case.
It reproduces with both dynamic and static pinned sanitizer runtimes. No leak
suppression or handshake-algorithm workaround was added. Diagnosis/fix of this
SSH allocation issue remains separate from the runtime-selection cutover; do
not describe the full sanitizer suite as green.

```sh
ctest --preset asan -R vectis_lua_ssh_sftp --output-on-failure
```

Evidence: `build/runtime-asan-tests.log` and
`build/runtime-asan-recheck.log`.

### Cutover sweep

The follow-up sweep added stale-manifest regression coverage, runtime mapping
checks for standalone pkg-config consumers and fuzz smoke, and the shared
runtime policy for native release-matrix SDK consumers. It removed unnecessary
Linux library-path environment overrides from SDK/release smoke checks.

- Fresh coverage build and complete suite: 89/89 passed
  (`build/runtime-sweep-coverage.log`).
- Standard debug build and complete suite: 90/90 passed, followed by formatting
  and lifecycle/privacy checks (`build/runtime-sweep-final.log`). The additional
  manifest regression was not part of the earlier timing comparison.
- Fresh AFL build and bounded seed smoke passed; the gate checked all 49 local
  executables (`build/runtime-sweep-fuzz.log`). This is not a long fuzz campaign.
- Static and shared SDK verification each checked three consumers (including
  pkg-config) and 36 installed examples; all passed
  (`build/runtime-sweep-sdk-static.log`, `build/runtime-sweep-sdk-shared.log`).
- Both runtime tests passed in the ASan configuration, including the sanitizer
  shared-library mapping probe. The full ASan suite was not repeated; the SSH
  leak above remains unresolved.

The earlier service e2e and release CLI results still apply to the unchanged
executable link policy. This sweep did not rebuild the full release matrix or
repeat service e2e. Cross-target execution remains outside the native policy.
