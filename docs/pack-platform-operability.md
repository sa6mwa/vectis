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

## Darwin Signing

Appending a payload changes executable bytes and invalidates an existing Mach-O
signature. Unsigned local packed artifacts do not need signing. When a signed
Darwin artifact is required, request signing during the final pack operation:

```sh
vectis -a pack --script app.lua --output app \
  --codesign "Developer ID Application: Example"
```

`--ad-hoc-codesign` is available for ad-hoc signatures. `--hardened-runtime`,
`--timestamp`, and `--entitlements <path>` require either signing option.
Vectis invokes the external Apple `codesign` program and verifies the final
artifact with `codesign --verify --strict --verbose=4`. The tool may be
overridden with `VECTIS_CODESIGN` for controlled build environments.
If signing or verification fails, Vectis removes the newly packed output rather
than leaving an unsigned artifact at the requested path.

Signing options are Darwin-only. Notarization and Gatekeeper assessment remain
separate release decisions and are not implicit side effects of packing.
For release verification, use
`scripts/verify_darwin_pack_signature.sh --binary <path>` and the packaged
Darwin smoke gate `scripts/verify_darwin_smoke_bundle.sh`.

## Supported Payloads

- one Lua script;
- optional lockd client certificate bundle material;
- optional embedded asset entries and content-type metadata;
- a shared JSON manifest with `format = "vectis-pack"`;
- extraction policies for embedded files.

Normal `vectis script.lua` execution remains unchanged when no valid embedded
payload is present.
