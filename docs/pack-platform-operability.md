# Vectis Pack Platform Operability

This document defines the operational limits for packed Vectis executables.
It covers the current Linux/ELF implementation and the required Darwin/Mach-O
contract before Darwin packed services can be treated as supported.

## Current Support

The implemented `vectis -a pack` path is Linux/ELF only. It copies the host
`vectis` runner, appends the Lua script, optional lockd client bundle, embedded
asset payload, LoneJSON pack manifest, and fixed `VECTIS_PACK` footer, then
executes the embedded Lua script after validating offsets, bounds, and hashes.

Linux packed binaries may contain:

- one Lua script payload,
- optional lockd client certificate bundle material,
- optional embedded asset entries,
- a shared JSON manifest using `format = "vectis-pack"`,
- a fixed footer that lets the runtime find and verify the payloads.

The Linux implementation does not codesign packed binaries and does not rely on
post-pack binary patching after the footer is written. Release privacy and
relocatability checks still apply to any packed artifact that becomes part of a
release. `vectis -a pack` accepts `--target host` and `--target native` as
explicit names for the current executable backend. Other Linux target IDs are
not accepted because the current ELF backend copies the running `vectis`
binary; it does not cross-pack a different Linux target from a host binary.

## Shared Manifest

The logical payload manifest is platform-independent. Platform-specific
containers may differ, but runtime behavior must continue to come from the same
manifest concepts:

- script metadata,
- lockd bundle metadata when present,
- embedded asset entries,
- aggregate asset tree behavior,
- default extraction policy,
- pack options that affect runtime startup.

ELF currently stores that manifest before the fixed footer. Darwin must expose
the same manifest bytes to the runtime, even if the bytes live in a generated
Mach-O section instead of appended executable data.

The detailed implementation contract is defined in
[Darwin Mach-O Pack Spec](darwin-mach-o-pack-spec.md). That spec recommends a
relink-based Darwin backend with Vectis-provided pack-runner link inputs rather
than an in-place Mach-O editor.

Binary SDKs now include the relocatable pack-runner input contract for that
future backend:

```text
share/vectis/pack-runner-link-inputs.json
lib/vectis/pack/libvectis_pack_runner.a
vectis::pack_runner
```

The manifest names the package-relative archive, CMake target, Mach-O section
contract, and finalization order. It must not contain source, build, cache, or
other workstation-local paths. `vectis -a pack --target arm64-apple-darwin`
looks for this installed SDK layout next to the running `vectis` binary or
under an explicit `--pack-sdk-root <root>`.

## Darwin Requirements

Darwin/Mach-O packed services are not supported until Vectis has a dedicated
Mach-O container implementation. That implementation must not rely on arbitrary
data appended after the executable image as the primary payload mechanism.

Required Darwin behavior:

- embed the Lua script, lockd bundle, assets, and shared manifest through a
  generated object or named Mach-O section layout,
- keep runtime validation independent of whether the container is ELF or
  Mach-O,
- discover and verify final Mach-O load commands with target-correct tools,
- avoid absolute non-system install names, dependency paths, or rpaths,
- avoid mutating a signed final executable,
- verify signed binaries with `codesign --verify --strict --verbose=4`,
- run `spctl --assess --type execute` when that tool is available and the
  artifact is intended for Gatekeeper assessment.

`scripts/verify_darwin_pack_signature.sh --binary <path>` is the reusable
verification command for a final signed Darwin packed executable. It runs
`codesign --verify --strict --verbose=4`, runs
`spctl --assess --type execute` when `spctl` is available, and can make `spctl`
mandatory with `--require-spctl`. The command hashes the binary before and
after verification and fails if either verification command mutates the file.
The command verifies final bytes only; it must run after every payload,
loader-metadata, strip, and signing step.

`.github/workflows/darwin-arm64-smoke.yml` is the hosted Apple Silicon smoke
surface for artifacts that have already been produced by the local release
pipeline. It is manual-only, runs on `macos-15` and `macos-latest`, downloads a
smoke-test zip either from a release asset or from an explicit HTTPS URL,
optionally checks its SHA-256, verifies extracted Mach-O signatures with
`codesign --verify --strict --verbose=4`, optionally requires
`spctl --assess --type execute`, and executes the bundle's `run-smoke.sh`
through `scripts/verify_darwin_smoke_bundle.sh`. The same verifier is the
on-device command for a real Mac:

```sh
scripts/verify_darwin_smoke_bundle.sh \
  --zip dist/vectis-<version>-arm64-apple-darwin-smoke-test.zip \
  --require-spctl
```

This verifies a packaged Darwin smoke zip; it does not replace the local
`make release-darwin-smoke-bundle` package generation gate. The on-device TODO
is complete only after this command has been run on real Apple Silicon hardware
against the candidate smoke zip.

## Signing Order

Signing is a finalization step. The pack pipeline must complete all payload
embedding, section generation, install-name/rpath work, stripping, and any
other byte mutation before invoking `codesign`.

The accepted order for Darwin packed executables is:

1. build or copy the unsigned runner,
2. embed pack payloads through the Mach-O container mechanism,
3. apply any required install-name, rpath, or strip operation,
4. sign or ad-hoc sign the final bytes,
5. verify the final artifact,
6. notarize or staple only after verification has passed.

No Vectis command may silently mutate the executable after signing. If a later
operation would change bytes, it must either run before signing or fail with an
actionable error.

## Signing Options

Darwin pack flags are explicit and validated before any output artifact is
created:

- `--codesign <identity>` signs with a named identity,
- `--ad-hoc-codesign` signs with an ad-hoc identity,
- `--hardened-runtime` requests hardened runtime options,
- `--timestamp` requests a timestamp when supported by the signing identity,
- `--entitlements <path>` supplies an entitlements plist.

`--codesign` and `--ad-hoc-codesign` are mutually exclusive. Hardened runtime,
timestamps, and entitlements are Darwin-only options and must fail clearly on
unsupported targets rather than being ignored. `--entitlements <path>` must be
a readable regular file.

`vectis -a pack --target arm64-apple-darwin` is routed to the Darwin/Mach-O
backend contract. Without a valid pack SDK, that command fails before creating
the output artifact with the documented `Darwin pack requires pack-runner link
inputs` diagnostic. With a valid `--pack-sdk-root`, the command validates the
manifest/archive pair and target id. If `--work-dir <dir>` is also supplied,
the current backend writes `vectis-pack-macho-sections.c` with `__VECTIS`
payload sections, then still fails before final output creation until
target-correct compile/link/sign support lands. This is deliberate: Darwin
payloads must not be produced by appending the Linux footer layout to a Mach-O
executable.

## Notarization Limits

Vectis pack may prepare a binary that is suitable for notarization, but it must
not hide notarization as an implicit side effect of packing. Notarization
requires Apple service credentials and a product release decision, so it belongs
in an explicit release or deployment step.

When notarization support is added, it must:

- be opt-in,
- refuse to run without explicit credentials/configuration,
- operate only on already signed and verified final artifacts,
- report Apple service failures as release blockers,
- never rewrite the packed executable after a successful verification without
  re-running verification.

## Stripping And Debug Data

Linux and Darwin packed binaries should avoid unnecessary debug metadata in
release artifacts. Darwin stripping is only allowed before signing, because
stripping a signed Mach-O can invalidate the signature. If stripping is
requested after signing, Vectis must fail and explain the required ordering.

Release verification remains responsible for scanning artifacts for local
source paths, build paths, dependency cache paths, hardening runtime paths,
credentials, and non-relocatable loader metadata.

## Unsupported Behavior

The following are intentionally unsupported:

- treating the current ELF footer as a Mach-O compatibility mechanism,
- appending data to Darwin executables and assuming Gatekeeper or codesign will
  accept it,
- signing before all payload bytes and loader metadata are final,
- mutating or stripping a signed final executable without re-signing and
  re-verifying,
- silently skipping Darwin signing verification for a shipped Darwin packed
  artifact,
- using host tools to inspect or mutate cross-built Mach-O artifacts when
  target-correct tools are available or required.
