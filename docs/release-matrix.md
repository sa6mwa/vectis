# Local release matrix

`make release-matrix` prepares Linux GNU/musl SDK and binary archives for
x86_64, aarch64 and armhf, source/Lua artifacts, checksums and artifact
verification. Use `make help` for narrower preparation targets. Preparation is
incremental; CMake owns matching build prerequisites. Changing toolchains or
migrating an old cache requires a fresh owned build directory.

The verifier extracts checksum-listed artifacts, checks static Linux CLI
payloads and target metadata, compiles downstream consumers, and checks privacy,
exports and relocatability. Static/shared consumer checks also cover the
separate PostgreSQL XA provider and app storage factories. Dependency SDKs stay
in independent `deps/<name>` prefixes with their own validation inventories.

`make release` first requires the actual default branch, a clean signed commit
and its exact lightweight release tag. It then owns the one full clean boundary
and local release pipeline. Preparation commands do not authorize release,
tagging or publication. The responsible agent must separately record the
semantic integration review required by `AGENTS.md`.

Darwin release build/link, Mach-O, SDK and smoke-bundle proof uses local prepared
osxcross through `make release-darwin-smoke-bundle`. The aggregate package
command currently includes Darwin when that prerequisite is available; an
explicit Darwin request fails if it is missing. A skipped Darwin build is no
Darwin release proof. Linux and Darwin runtime evidence remain distinct.

`.github/workflows/darwin-kqueue.yml` is an optional native diagnostic lane for
kqueue, HTTP/SSE/WebSocket/WSS, shutdown and Lua lifetimes. It uses manual
dispatch only. Hosted runs and their diagnostic artifacts are never required
release gates or release artifacts. A local osxcross build proves build/link
and package behavior; it does not claim native macOS runtime execution.
Dispatching, pushing or publishing requires the user's authorization.
