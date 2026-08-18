# Darwin Mach-O Pack Spec

Vectis currently implements `vectis -a pack` for Linux/ELF by copying the
running executable and appending a validated `VECTIS_PACK` footer. Darwin must
not reuse that layout. Darwin packed executables need payload bytes inside a
Mach-O container, all byte mutation must finish before signing, and verification
must inspect final extracted artifacts with target-correct tools.

This spec defines the implementation shape for the remaining Darwin pack work.
It does not mark Darwin packed services as supported until the code and gates
described here exist.

## Goals

- Preserve the existing logical pack format: one Lua script, optional lockd
  client bundle, optional embedded assets, and one shared LoneJSON manifest with
  `format = "vectis-pack"`.
- Keep runtime validation independent of the platform container. The Lua
  script, bundle, asset payload, and manifest must still be bounds-checked and
  SHA-256 verified before execution or exposure through `vectis.embedded`.
- Embed Darwin payloads through named Mach-O sections, not arbitrary appended
  executable data.
- Keep `vectis -a pack` as the user-facing command and preserve the existing
  pack flags where the behavior is platform-independent.
- Add Darwin signing flags only after payload embedding and all other byte
  mutation are final.
- Verify signed final bytes with `codesign --verify --strict --verbose=4` and
  optional `spctl --assess --type execute`.

## Non-Goals

- No compatibility mode for the Linux/ELF footer on Darwin.
- No hidden notarization. Notarization remains a separate explicit release or
  deployment operation.
- No post-signing strip, install-name, rpath, section, or payload mutation.
- No Darwin pack support that silently skips target-correct Mach-O inspection
  when a Darwin artifact is produced.

## Recommended Architecture

Darwin should use a relink-based pack backend rather than an in-place Mach-O
editor.

The pack command is being split into three layers:

- `vectis_pack_collect`: shared C collection logic that reads the Lua script,
  lockd bundle, assets, content-type map, extract policy, and manifest into a
  platform-neutral `vectis_pack_payload`.
- `vectis_pack_write_elf`: the existing Linux writer, now behind a backend
  boundary and still responsible for the copied executable plus footer layout.
- `vectis_pack_write_macho`: a Darwin backend that writes a generated source or
  object containing named sections and links a final executable from the
  pack-capable runner link inputs.

The Darwin backend should generate these section payloads:

```text
__VECTIS,__pack_header
__VECTIS,__pack_script
__VECTIS,__pack_bundle
__VECTIS,__pack_assets
__VECTIS,__pack_manifest
```

`__pack_header` must include magic, layout version, byte lengths, SHA-256
digests, and a manifest schema version. Offsets inside the asset payload remain
manifest-owned so the same embedded asset parser can be reused.

The runtime should add a Darwin payload loader that reads its own Mach-O image
sections and returns borrowed memory ranges for the same validation path used by
Linux. On Darwin, this should use the system Mach-O APIs from the running main
image, normally anchored from `_mh_execute_header`, rather than scanning
arbitrary files on disk.

## Relink Backend Contract

A final Mach-O executable cannot be safely transformed into a packed executable
by appending data. It also should not be treated as a generic mutable container
after signing. Therefore the supported Darwin packer should relink a final
runner from known link inputs.

The implementation should provide one of these repository-owned link inputs:

- a pack-runner object archive produced by the Darwin release build, or
- a CMake response/manifest file listing the exact target-correct objects,
  libraries, frameworks, rpaths, install-name policy, and linker flags needed
  to link the `vectis` executable.

Vectis now packages the first form as an installed SDK contract:

```text
share/vectis/pack-runner-link-inputs.json
lib/vectis/pack/libvectis_pack_runner.a
lib/cmake/vectis/vectisConfig.cmake target vectis::pack_runner
```

The JSON manifest is relocatable and records the target id, runner archive,
CMake target name, required Mach-O section names, and finalization order. It is
not a full Darwin pack implementation by itself; the pack command still needs a
backend that compiles the generated section object, links it with
`vectis::pack_runner`, signs final bytes, and verifies the result.

`vectis -a pack` on Darwin should discover those inputs from the installed SDK
layout next to the running `vectis` binary, or from an explicit
`--pack-sdk-root <root>` that contains the same installed SDK layout. If they
are unavailable, it must fail before creating an output artifact with an
actionable diagnostic such as:

```text
vectis: Darwin pack requires pack-runner link inputs; build or install the
arm64-apple-darwin pack SDK before using signing options
```

This means Darwin packing is not fully self-contained in the same way as Linux.
It requires target-correct link tools and Vectis-provided runner link inputs at
pack time. That is the recommended product contract because it avoids an
in-place Mach-O editor and keeps code signing order deterministic.

Until `vectis_pack_write_macho` is implemented, valid SDK inputs are treated as
a validated precondition and the command still fails before output creation with
an explicit unsupported-backend diagnostic. Target mismatches in
`pack-runner-link-inputs.json` fail before reaching the backend.

## Signing Contract

The Darwin pack command should accept:

- `--codesign <identity>`
- `--ad-hoc-codesign`
- `--hardened-runtime`
- `--timestamp`
- `--entitlements <path>`

`--codesign` and `--ad-hoc-codesign` are mutually exclusive. Hardened runtime,
timestamp, and entitlements are only meaningful when signing is requested.
If any signing-only option is present without `--codesign` or
`--ad-hoc-codesign`, the command should fail with a usage diagnostic.
`--entitlements <path>` must name a readable regular file and should be
validated before backend selection so missing files are reported as input
errors rather than as generic unsupported-platform failures.

The only valid signing order is:

1. collect and hash pack payloads;
2. generate section source/object material;
3. link the unsigned packed runner;
4. apply any required target-correct pre-signing install-name/rpath/strip step;
5. sign or ad-hoc sign the final bytes;
6. run signature verification;
7. optionally run Gatekeeper assessment;
8. publish the output path.

No command may mutate the output after step 5 unless it repeats signing and
verification before returning success.

## Tool Discovery

Darwin pack must use `scripts/discover_target_tools.sh` or the same lookup
contract:

- prefer configured CMake target tools from the Darwin build directory;
- prefer osxcross-prefixed sibling tools such as
  `arm64-apple-darwin25-otool`;
- prepend `${OSXCROSS_ROOT}/bin` when invoking target link commands;
- never use host Linux `strip`, `install_name_tool`, `otool`, or linker tools
  for a cross-built Mach-O artifact.

On native macOS, unprefixed Apple tools may be used only after the target is
confirmed to be Darwin and the output is a native Mach-O artifact.

## Verification Plan

Local deterministic gates should include:

- a backend dispatch test proving Linux uses the ELF writer and Darwin refuses
  to pack without link inputs;
- a generated-section fixture test that compiles or inspects a tiny generated
  Mach-O object when target tools are available;
- a signing-argument parser test for mutually exclusive identities, missing
  signing mode, entitlements, timestamp, and hardened runtime usage;
- a signing-order test with fake tools proving no output is left behind after
  failure before publish;
- an extracted-artifact privacy test that fails on local Mach-O load paths and
  signature-invalidating post-sign mutation risks;
- hosted Apple Silicon smoke using `.github/workflows/darwin-arm64-smoke.yml`
  after a smoke zip exists.

The existing `scripts/verify_darwin_pack_signature.sh` remains the final-byte
signature verifier. Package verification remains responsible for Mach-O load
commands, install names, dependency paths, rpaths, privacy, and relocatability.

## Open Decision

The recommended implementation requires Darwin pack-time link inputs and
target-correct link tools. If Darwin `vectis -a pack` must instead be
self-contained like Linux, then Vectis needs an in-place Mach-O section editor
with complete load-command resizing, alignment, code-signature invalidation,
re-signing, and verification semantics. That is a materially different
implementation and should not be started without an explicit product decision.
