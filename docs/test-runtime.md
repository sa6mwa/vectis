# Pinned Bootlin runtime for local executables

Dependency preparation keeps cpkt, cpktdb, cpktmisc and other SDKs in separate
prefixes. `vectis_dependency_cache` exercises real local HTTP acquisition,
verified hits with zero requests, corruption repair, asset renaming by digest,
checksum rejection, preserved payload modes and borrowed-cache rejection.
Toolchain and archive overrides may use this checkout's build/.cache or their
declared global cpkt caches; configuration and provisioning reject foreign
checkout paths, including symlinks whose final directory does not yet exist.

Local GNU/Linux x86-64 executables use the configured Bootlin interpreter and
runtime libraries directly. No Bubblewrap, container, CTest launcher, or duplicate
test CLI is involved. The current collection is stable-2026.08-1 (glibc 2.44).

## Artifact boundary

| Artifact | Runtime |
| --- | --- |
| Debug CLI, tests, compiled helpers, examples, fuzzers, benchmarks | Pinned Bootlin interpreter and runtime search paths |
| Linux Release CLI | Statically linked; normal distributable artifact |
| Shipped libraries and SDK metadata | No local-executable runtime settings |
| Go fixtures (CGO disabled), Python data fixtures, CMake, shell, Valgrind | Host runtime |

`cmake/test_runtime.cmake` applies private executable link settings after target
creation. DT_RPATH covers transitive dependencies as well as direct dependencies.
The configured sysroot is the source of paths, not a duplicated cache location.
Library targets and their exported usage requirements are not modified.

Linux configuration must select a toolchain explicitly, normally through a
checked-in preset. A plain `cmake -S . -B ...` without a toolchain is rejected
before compiler discovery. The toolchain supplies the default target identity,
so selecting it without repeating `VECTIS_TARGET_ID` still enables the runtime
policy. No-Kore, install-tree and extracted-source smoke builds use the same
presets as the ordinary debug/release builds.
An explicitly supplied target identity that conflicts with the selected
toolchain is rejected rather than disabling the matching runtime policy.

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

Runtime mapping and reconfiguration checks now run as native CMake/CTest
checks; Python remains limited to declared source/data generation and small
data fixtures. Kore probes use `tests/run_kore.cmake` for native process
execution and sanitizer-report capture. There is no PID/process-group broker
or process-table scanning wrapper. The embedded runtime's control socket
handles controller death; its compile guard is enabled in the ordinary build.
The abrupt controller-exit regression passes in 0.28 seconds, without a
synthetic timeout or retry delay. Earlier wrapper evidence below is historical.

The runtime contract enumerates configured local executable targets, checks their
ELF interpreters, and traces resolved libraries with inherited `LD_*` settings
removed. Toolchain-provided dependencies must resolve to the pinned files;
unexpected host dependencies fail the gate. It also checks child self-exec and
includes a missing-runtime negative fixture. A Release run additionally checks
that the CLI has no interpreter or dynamic dependencies.

Every loader mapping is checked, including absolute-path `DT_NEEDED` entries
printed without `=>`. Unknown output and non-absolute mappings are rejected.
A compiled regression covers both named and absolute dependencies, paths with
spaces, allowed versus forbidden library roots, and substituted pinned libc.

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

`make package-verify` runs the release privacy verifier over extracted artifacts,
including recursively nested archives. Every ELF interpreter must be one of the
normal system loader paths for the supported Linux architectures; arbitrary
paths under a Bootlin sysroot are forbidden regardless of cache location.
RPATH/RUNPATH entries must be `$ORIGIN`-relative, and `DT_NEEDED` entries cannot
contain paths. The shipped Linux CLI must still be fully static. Missing ELF
inspection tools, unreadable ELF metadata and failed nested extraction fail the
gate rather than skipping inspection. Real compiled fixtures cover system and
Bootlin interpreters, both runtime-path tags, absolute dependencies and static
binaries inside multiply nested release archives. Scanner scratch stays under
`build/`.

## Historical runtime-selection verification

Current split-bundle SDK checks additionally pin `libresolv.so.2` as a direct
local shared-consumer dependency. Provider RUNPATHs otherwise prevent ancestor
RPATH lookup and can select the host's older resolver library despite the
pinned loader/libc. This policy applies only to local shared SDK consumers and
examples; it is not exported in `libvectis` or provider packages. The mapping
gate still checks the actual loaded file against the selected Bootlin runtime.

The measurements and dependency-refresh records below describe their dated
candidates. Their test counts, provisional failures and delivered binary digest
are historical evidence, not the current suite or release status. The subsequent
[Lua integration audit](lua-upgrade-seam-audit.md#completed-independent-review-and-final-verification)
records the 211-check candidate and closure checks. The old Darwin-workflow
policy conflict was then resolved by an adopted-workflow allowlist. The current
split-bundle lifecycle supersedes that policy: hosted Darwin is optional
diagnostics, while release proof uses local osxcross. Native execution is not
claimed by cross compilation. Current memory evidence is in the active audit.
The SSH sanitizer finding was not requalified by the targeted source tests.

## Measurement method

Measure execution, excluding the build, in a fresh cgroup for each run:

```sh
systemd-run --user --wait --pipe --property=CPUAccounting=yes --property=MemoryAccounting=yes \
  --working-directory="$PWD" --unit=vectis-runtime-measure-1 \
  /usr/bin/time -v ctest --preset debug
```

Use a new unit name for each run. systemd/cgroup v2 is a measurement dependency,
not a test execution dependency. Use systemd's final accounting for aggregate
cgroup CPU and memory peak, and `time` for elapsed time and child user/system CPU.
Cgroup memory includes charged file cache; `time`'s maximum RSS is a different
measurement. The historical JSON records below came from the retired Python
controller and include cgroup swap peak as well. Results describe this workload
and machine, not a universal fixed overhead.

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

### Auxiliary-entry-point sweep

A second sweep reproduced host compiler selection and missing runtime-test
registration through a plain CMake configuration. The no-Kore, install-tree
and source-archive smoke entry points were still using that route. They now
select the existing presets; the startup guard and toolchain-derived identity
prevent that accidental fallback. The regression also covers conflicting target
identities and Release test configurations with the CLI disabled.

- `make test-no-kore` passed, including runtime checks for 37 executables.
- A fresh `make test-install-tree` passed direct-install and packaged static/
  shared SDK checks. Its two Release runtime tests passed, including verification
  that the CLI has no interpreter or dynamic dependencies.
- `make package-source-smoke` extracted, built and passed all 89 tests using
  Bootlin from the source archive.
- Final debug suite: 90/90. Formatting, lifecycle/privacy, resolver and shell
  checks passed. Both ASan runtime tests passed; the full sanitizer suite and
  service e2e were not repeated, and the SSH leak remains unresolved.

Evidence: `build/runtime-sweep2-no-kore.log`,
`build/runtime-sweep2-install.log`, `build/runtime-sweep2-source.log`, and
`build/runtime-sweep2-final.log`. The old host-built install-tree cache was
preserved at `build/runtime-sweep2-host-install-tree` before the fresh build.

### Mapping-gate sweep

A compiled probe exposed a verifier blind spot: absolute dependencies printed
without `=>` were ignored. The checker now validates every mapping and rejects
unrecognized output. Both loader formats have positive and negative regressions.
No wrong-runtime mapping was found in the existing debug, ASan, coverage, fuzz,
no-Kore or Release install-tree builds under the stricter checker.

The debug suite passed 91/91, followed by formatting and lifecycle/privacy
checks (`build/runtime-sweep3-final.log`). Shared SDK verification passed for
three consumers and 36 installed examples (`build/runtime-sweep3-sdk.log`).
This sweep changed no executable link policy. Full ASan and service e2e were
not repeated; the documented SSH sanitizer leak remains unresolved.

### c.pkt.systems 0.10.0 upgrade

The shared-deployment glibc minimum increase from 2.38 to 2.43 was approved
on 2026-09-14. All seven SDK target pins use the upstream 0.10.0 release
checksums; other dependency versions and public facade ABI majors are unchanged.
Consumers of the shared GNU/Linux dependency set must use glibc 2.43 or newer.
The static Linux CLI remains independent of that dynamic-runtime requirement.

The upgrade passed all 91 debug tests, direct-install and packaged static/shared
SDK consumer and installed-example checks, lifecycle checks, and the top-level
package verifier against the generated x86_64 GNU archive. The verifier included
the ELF interpreter and loader-path guards described above. Evidence is in
`build/cpkt-010-trial.log`, `build/cpkt-010-install.log`,
`build/cpkt-010-package-verify.log` and `build/cpkt-010-lifecycle.log`.
This was native x86_64 GNU verification, not a full cross-target release matrix,
service e2e, sanitizer or Valgrind rerun. No release was published.

### Dependency refresh on 2026-10-03

The pinned release set now uses c.pkt.systems 0.11.0, lonejson 0.45.0,
softline 0.9.0, and libpid0 0.5.0. Libpslog 0.12.0, CAI 0.6.0,
liblockdc 0.18.0, liblql 0.3.0, and libmdf 0.13.0 were already at their latest
published releases. All 62 SDK and Linux libpid0 asset pins across the seven
targets match the official release asset SHA-256 digests. Lua source pins for
the upgraded bindings match their published checksum manifests.

Kore is pinned to `1ff3c0f6f7973dc7fc8bf38369790ef2d8079b3b`. Patch 0037
preserves its HTTP header-length underflow protection while retaining Vectis's
pipelined request handling. The clean patch-application/build verifier and
the pipelining and prebody handoff tests passed. Smith's Alt-Enter binding
uses Softline's public key and queue APIs to preserve immediate steering under
the new host-controlled queue delivery contract. Its new synchronous binding
checks and existing PTY integration tests passed without adding test delays.

The native debug fast gate passed 23/23 in 2.30 seconds, including all 38
lonejson encoder checks. The remaining standard debug suite initially passed
177/178; its Smith queue failure identified the integration change above.
After that correction, all eight selected CLI/Smith tests passed. Every test
in the 201-test debug suite therefore passed across the standard run and
focused reruns. Formatting and shell syntax checks passed.

Direct-install and packaged static/shared SDK checks passed, including the
C, C++, and pkg-config consumers and 36 installed examples per mode. The
new stripped x86_64 musl static CLI passed the supplied production probe's
69/69 checks in script, packed, and direct modes, all with clean shutdown.
Separate encoder shape and unsupported-session cleanup probes also passed.
The binary delivered during that dependency-refresh task had SHA-256
`0d07934b6a2c27d22b6af7ebad5e521c820a9fac9a1d1476627d1002f58e8625`.

At that point the lifecycle contract script stopped at a policy conflict: it forbade
repository automation files, but the branch tracks
`.github/workflows/darwin-kqueue.yml`. That policy assertion and workflow were
left unchanged in that dependency-refresh task; the later integration cutover
resolved this assertion. The dependency pins were verified independently. The shared
GNU/Linux glibc minimum remains 2.43 and public facade ABI majors are unchanged.
Cross-target runtime, full service e2e, sanitizer, and Valgrind suites were not
rerun. Evidence is retained under `build/dependency-upgrade/`. No release was
published.
