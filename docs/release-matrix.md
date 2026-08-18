# Local Release Matrix

`make release-matrix` is the local lifecycle surface for the full GNU and musl
release matrix. It builds the supported Linux GNU/musl SDK and binary artifacts,
creates the source archive and Lua release artifacts, generates the checksum
manifest, and runs `verify-release-matrix` with:

```sh
VECTIS_REQUIRE_LINUX_RELEASE_MATRIX=1 make release-matrix
```

The verifier extracts every checksum-listed artifact, checks static `vectis`
executable payloads, validates ELF target metadata, builds downstream SDK
consumers for each Linux target, and runs release privacy and relocatability
checks.

Vectis uses the pkt.systems local lifecycle model. The release matrix is run
locally through Make and the repository must not define remote workflow
automation for it.
