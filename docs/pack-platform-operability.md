# Vectis Pack Platform Operability

`vectis -a pack` creates a runnable copy of the `vectis` executable that
invoked it. It appends one Lua script, an optional lockd client bundle,
optional embedded assets, the shared JSON manifest, and a fixed `VECTIS_PACK`
footer. The packed executable therefore has the same operating system,
architecture, and linked runtime as its input executable.

Packing is self-contained: it does not cross-compile, relink, use an SDK, or
invoke CMake. There is no target-selection option. To make an artifact for a
different platform or architecture, run `vectis -a pack` using a Vectis binary
for that platform or architecture.

## Runtime Format

The footer stores offsets, lengths, and SHA-256 digests for the script, lockd
bundle, assets, and manifest. At startup Vectis reads its own executable,
validates the footer, bounds, and hashes, and only then exposes the embedded
payloads or executes the embedded script.

This format is used on Linux and Darwin. Darwin first reads the footer; this
allows a packed artifact to remain the original Mach-O executable plus its
payload, rather than requiring a target SDK or a relink step.

## Darwin

Darwin packing uses the same tool-free copy-and-append format as Linux. It does
not invoke `codesign`, CMake, a linker, an SDK, or any platform inspection
tool. Pack deliberately has no signing, notarization, entitlement, or
Gatekeeper options: it produces a local runnable artifact for the executable
that invoked it.

## Supported Payloads

- one Lua script;
- optional lockd client certificate bundle material;
- optional embedded asset entries and content-type metadata;
- a shared JSON manifest with `format = "vectis-pack"`;
- extraction policies for embedded files.

Normal `vectis script.lua` execution remains unchanged when no valid embedded
payload is present.
