# Kore Runtime Packaging

## Decision

Vectis keeps Kore as a vendored upstream checkout plus a Vectis-owned patch
stack under `vendor/kore`, and builds that patched source as a private
implementation detail. In release packaging terms, Kore is a private Vectis-owned object runtime.

Release artifacts must not require an external Kore installation. They must not
ship or require a standalone public `libkore` artifact, Kore runtime modules, or
user-provided Kore config files. The selected patched Kore objects are folded
into the Vectis build graph through the private `vectis_kore_runtime` object
target and are consumed by `libvectis` and the `vectis` executable as
implementation detail.

## Build Contract

- `vendor/kore/REVISION` records the exact Kore commit and `vendor/kore/upstream`
  is the readable checkout of that revision.
- `vendor/kore/patches/series` is the ordered Vectis patch stack.
- `scripts/verify-kore-patches.sh` verifies that the patch stack still applies
  cleanly and builds against the provisioned SDK root.
- `CMakeLists.txt` owns the product build: it selects the Kore sources Vectis
  supports, creates generated Kore source files under the build directory, builds
  `vectis_kore_runtime` as an `OBJECT` target, and appends those objects to the
  Vectis source list.
- Installed SDK archives expose Vectis public headers and dependency closure
  metadata. They do not expose Kore as an independent public runtime ABI.

## Product Boundary

Kore remains the embedded HTTP runtime used by Vectis server workflows, but
Vectis is the public product surface. Product behavior is exposed through
`include/vectis/vectis.h`, the `vectis` Lua runtime, examples, and docs. Native
Kore config syntax, dynamic Kore modules, and standalone Kore packaging are not
public Vectis interfaces unless a future Vectis-owned API explicitly adds them.

This keeps Kore upgradeable as a patch stack while keeping release artifacts
self-contained and relocatable.
