# Vectis Lifecycle Migration

This ledger tracks the migration toward the pkt.systems C/CMake lifecycle. It is
temporary until the repository has fully converged or the engineer decides to
keep it as project documentation.

| Old command or behavior | Lifecycle surface | Behavior preserved | Verification added | Status |
| --- | --- | --- | --- | --- |
| Hard-coded CMake/project and CLI version `0.0.0` | `scripts/release_version.sh`, generated `vectis_version.h`, `make print-release-version` | Git worktrees resolve by exact lightweight `vX.Y.Z` tag first, then explicit `VECTIS_VERSION_OVERRIDE`, then `0.0.0`; root `/VERSION` is ignored in git and only source archives consume injected `VERSION` | `scripts/test_lifecycle_contracts.sh` | Implemented |
| Release presets infer target id from arch/os/libc | Explicit `VECTIS_TARGET_ID` preset cache values | Existing target IDs remain unchanged; the old inference remains a fallback for ad hoc builds | `scripts/test_lifecycle_contracts.sh` | Implemented |
| `build-asan`, `build-fuzz`, `build-coverage` only | Standard `asan`, `fuzz`, `fuzz-smoke`, `coverage` aliases | Existing build targets remain available | `make asan`, `make fuzz-smoke`, `make coverage` | Implemented |
| Binary SDK install omitted project `LICENSE` and `README.md` | Binary SDK documentation contract under `share/doc/vectis` | Existing examples and dependency docs remain installed | `scripts/verify_release_artifacts.sh` | Implemented |
| No source release artifact contract | `make package-source`, `make package-source-smoke` | Source archive stages tracked/nonignored files plus injected `VERSION` and `RELEASE_MANIFEST` | `scripts/test_release_from_source.sh` | Implemented |
| Package, Darwin smoke, and verification had separate tool lookup paths | `scripts/discover_target_tools.sh` | Configured compiler/cache state remains authoritative; Darwin cross artifacts refuse generic host mutation/inspection tools | `scripts/test_discover_target_tools.sh` | Implemented |
| Optional cross targets failed late when the compiler existed but the target SDK was incomplete | `scripts/target_toolchain_available.sh` and `PKT_DIAGNOSTIC_BEGIN` diagnostics | Direct target requests still fail; aggregate matrix skips unavailable optional targets with `external-tool-unavailable` | `scripts/test_lifecycle_contracts.sh`, `make release-matrix` | Implemented |
| Bundled libxml2 default catalog and file paths embedded dependency cache paths | Dependency provisioning / release privacy | XML parser dependency remains bundled; catalog support disabled, dependency pkg-config files are rewritten relative to package root, and compiler file-prefix maps are applied for relocatable SDKs | `scripts/verify_release_privacy.sh` | Implemented |
| Target packages were not verified from a checksum manifest | `make package-checksums`, `make package-verify`, `make verify-release-archives`, `make verify-release-privacy` | Existing package archive naming remains unchanged; negative privacy fixtures prove representative local-path, file-URL, and non-relocatable RPATH leaks fail | `scripts/verify_release_artifacts.sh`, `scripts/verify_release_privacy.sh`, `scripts/test_release_privacy_contracts.sh` | Implemented |
| CAI was not part of the lifecycle dependency graph | Pinned CAI 0.1.2 SDK archives plus pinned LockDC, CAI, and LoneJSON source rocks for embedded Lua C bindings | The Vectis SDK exposes CAI through installed CMake dependency closure, while the `vectis` runner statically links CAI and preloads `require("lockdc")`, `require("cai")`, and `require("lonejson")` without invoking LuaRocks | `make test-debug`, `make test-install-tree`, `make package-verify` | Implemented |

Open decisions:

- Whether Vectis will ship Lua release artifacts now, or only keep current Lua
  runner tests until a dedicated Lua facade/release slice.
