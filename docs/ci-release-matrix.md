# Release Matrix CI

`.github/workflows/linux-release-matrix.yml` is the hosted Linux CI surface for
the full GNU and musl release matrix. It runs on `ubuntu-24.04`, installs only
host prerequisites, restores the lifecycle-owned `${HOME}/.cache/c.pkt.systems`
dependency and toolchain cache roots, and delegates product verification to:

```sh
make release-matrix
```

That target builds the supported Linux GNU/musl SDK and binary artifacts,
creates the source archive and Lua release artifacts, generates the checksum
manifest, and runs `verify-release-matrix` with
`VECTIS_REQUIRE_LINUX_RELEASE_MATRIX=1`. The verifier extracts every
checksum-listed artifact, checks static `vectis` executable payloads, validates
ELF target metadata, builds downstream SDK consumers for each Linux target, and
runs release privacy and relocatability checks.

The workflow uploads `dist/` only after the matrix passes. Release publication
still uses the local lifecycle release flow; CI artifacts are evidence for the
candidate, not a publishing authority.
