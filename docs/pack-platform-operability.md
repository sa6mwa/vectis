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
release.

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

Future Darwin pack flags must be explicit:

- `--codesign <identity>` signs with a named identity,
- `--ad-hoc-codesign` signs with an ad-hoc identity,
- `--hardened-runtime` requests hardened runtime options,
- `--timestamp` requests a timestamp when supported by the signing identity,
- `--entitlements <path>` supplies an entitlements plist.

`--codesign` and `--ad-hoc-codesign` are mutually exclusive. Hardened runtime,
timestamps, and entitlements are Darwin-only options and must fail clearly on
unsupported targets rather than being ignored.

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
