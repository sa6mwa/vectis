# Vectis Pack Platform Operability

On Linux, `vectis -a pack` creates a runnable copy of the `vectis` executable
that invoked it. It appends one Lua script, an optional lockd client bundle,
optional embedded assets, the shared JSON manifest, and a fixed `VECTIS_PACK`
footer. The packed executable therefore has the same architecture and linked
runtime as its input executable.

Packing is self-contained: it does not cross-compile, relink, use an SDK, or
invoke CMake. There is no target-selection option. To make an artifact for a
different Linux architecture, run `vectis -a pack` using a Vectis binary for
that architecture.

## Runtime Format

The footer stores offsets, lengths, and SHA-256 digests for the script, lockd
bundle, assets, and manifest. At startup Vectis reads its own executable,
validates the footer, bounds, and hashes, and only then exposes the embedded
payloads or executes the embedded script.

This format is supported on Linux.

## Darwin

`vectis -a pack` is unavailable on Darwin. Apple Silicon requires executable
code signatures, and appending a payload to the linked Mach-O invalidates its
embedded signature. Vectis deliberately does not invoke `codesign` or expose
signing, notarization, entitlement, or Gatekeeper options, so it refuses the
operation before reading inputs or creating an output artifact. Apple
documents both the Apple Silicon signing requirement and the need to re-sign
executables modified after linking in the
[macOS Big Sur 11.0.1 Universal Apps release notes](https://developer.apple.com/documentation/macos-release-notes/macos-big-sur-11_0_1-universal-apps-release-notes/).

## Supported Payloads

- one Lua script;
- optional lockd client certificate bundle material;
- optional embedded asset entries and content-type metadata;
- a shared JSON manifest with `format = "vectis-pack"`;
- extraction policies for embedded files.

Normal `vectis script.lua` execution remains unchanged when no valid embedded
payload is present.
