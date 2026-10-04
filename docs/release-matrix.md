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


The already declared native Darwin verification lane is
`.github/workflows/darwin-kqueue.yml` in `sa6mwa/vectis` (`origin`). It targets
`darwin-debug` / arm64 Apple Darwin on the workflow's `macos-26` runner and checks
native kqueue HTTP, SSE, WebSocket/WSS and shutdown behavior, plus native
supervisor FD ownership and fast Lua supervisor/workflow/OPC UA lifetimes
with serialized builds/tests. Linux packed cases remain conditional. Preserve this
required platform evidence; Linux or cross-build results do not substitute for
it. This workflow verifies the native platform and does not run or publish the
local release matrix. The lifecycle allowlist permits only this adopted file.

No additional repository automation is adopted. Running the hosted lane requires
applicable publication/trigger authority; this audit task authorizes no push,
dispatch or release. Native Darwin execution remains an unrun platform gate in
this local Linux verification record.
