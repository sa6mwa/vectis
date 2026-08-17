# Vectis Lua Surface

The `vectis` executable embeds the cpkt Lua runtime and preloads both
dependency-native facades and Vectis-owned workflow helpers. The repository
also carries a pure Lua top-level `vectis` module and standalone LuaRocks
source artifacts so the Lua helper namespace can be loaded from an ordinary
Lua 5.5 environment.
Dependency-native modules should stay thin over their C implementation.
Vectis-owned modules should provide the service/application DX where a workflow
crosses Vectis concepts such as Kore routes, packed assets, auth, lockd
consumers, certificate workflows, or file-backed transfers.

For release tracking, see the [Lua coverage matrix](lua-coverage-matrix.md).
For shared naming, error, ownership, and payload-shape rules, see the
[Lua facade conventions](lua-conventions.md).
For SUS/audio callback and ownership details, see the
[SUS and audio contract](lua-sus-audio-contract.md).
For OPC UA callback and concurrency details, see [Lua OPC UA](lua-opcua.md).

## Framework Model

The Lua framework has two deliberate layers:

- Dependency-native modules expose bundled libraries directly through
  `require("<module>")`. They keep upstream naming and complete API coverage
  where a dependency already has a useful Lua facade.
- Vectis-owned modules live under `vectis.*` and compose Vectis concepts such
  as server routes, auth, packed assets, lockd workflow cleanup, terminal DX,
  certificate workflows, HTTP/WebDAV/MQTT/SMTP defaults, and structured status
  metadata.

`require("vectis").libs` is an alias table for bundled dependency modules, not
a wrapper layer. Do not add a Vectis-owned helper solely to mirror a dependency
API. Add one only when it removes real Vectis service-workflow friction, crosses
multiple Vectis-owned concepts, or gives a safer ownership/cleanup boundary.

## Dependency-Native Modules

These modules are intended to expose upstream or bundled C library behavior
with minimal Vectis opinion:

- `lockdc`: upstream liblockdc Lua module.
- `lonejson`: upstream LoneJSON Lua module.
- `pslog`: upstream libpslog Lua module, documented in
  [Lua pslog](lua-pslog.md).
- `lql`: upstream liblql Lua module, documented in [Lua lql](lua-lql.md).
- `cai`: upstream CAI Lua module, documented in [Lua CAI](lua-cai.md).
- `libmdf`: upstream libmdf Lua module, documented in
  [Lua libmdf](lua-libmdf.md).
- `softline`: upstream softline Lua module, documented in
  [Lua softline](lua-softline.md).
- `curl`: generic libcurl facade, documented in [Lua curl](lua-curl.md).
- `openssl`: OpenSSL facade, documented in [Lua OpenSSL](lua-openssl.md).
- `zlib`: zlib/gzip compression facade, documented in
  [Lua zlib](lua-zlib.md).
- `opcua`: cpkt OPC UA facade, documented in [Lua OPC UA](lua-opcua.md).
- `audio`: cpkt audio/miniaudio facade, documented in
  [Lua audio](lua-audio.md).
- `sus`: cpkt SUS/whisper facade, documented in [Lua SUS](lua-sus.md).

The C SDK artifacts intentionally do not ship the embedded Lua runtime, Lua
source tree, or Lua package-manager state. Standalone Lua release artifacts are
published separately as `vectis-lua-<version>.tar.gz`,
`vectis-<version>-1.rockspec`, and `vectis-<version>-1.src.rock`.

`require("vectis").libs` collects these bundled library facades under one
namespace for applications that already use the top-level Vectis module:
`vectis.libs.lockdc`, `vectis.libs.lonejson`, `vectis.libs.pslog`,
`vectis.libs.lql`, `vectis.libs.cai`, `vectis.libs.libmdf`,
`vectis.libs.softline`, `vectis.libs.curl`, `vectis.libs.opcua`,
`vectis.libs.openssl`, `vectis.libs.zlib`, `vectis.libs.audio`, and
`vectis.libs.sus` are the same module tables returned by direct
`require(...)`. Direct `require(...)` remains the canonical way to load an
individual dependency facade.

## Vectis Workflow Modules

These modules are Vectis-owned helpers over one or more dependency-native
modules or C SDK workflows:

- `vectis`: top-level runtime helpers and module namespace.
- `vectis.auth`: Lua DX helpers plus the native auth, TOTP/QR, email token,
  OAuth2/OIDC, and WebDAV key facade, documented in
  [Lua auth](lua-auth.md).
- `vectis.audio_worker`: managed audio worker mailbox envelope helpers for
  bounded file decode/encode work and VOX state/segment events, documented in
  [Lua audio](lua-audio.md).
- `vectis.cai`: CAI service configuration, owned/borrowed client and agent
  helpers, and structured CAI error decoration, documented in
  [Lua CAI](lua-cai.md).
- `vectis.cai_worker`: managed CAI worker mailbox envelope helpers, documented
  in [Lua CAI](lua-cai.md).
- `vectis.sus_worker`: managed SUS worker mailbox envelope helpers for bounded
  PCM/file transcription work, documented in [Lua SUS](lua-sus.md).
- `vectis.cert`: certificate/key/CSR/CA workflows, documented in
  [Lua certificates](lua-certs.md).
- `vectis.curl_worker`: managed curl worker mailbox envelope helpers,
  documented in [Lua Curl](lua-curl.md).
- `vectis.dsv`: CSV/TSV/DSV parsing and serialization, documented in
  [Lua DSV](lua-dsv.md).
- `vectis.embedded`: packed embedded asset inspection, reading, chunking,
  listing, and extraction helpers, documented in
  [Lua embedded assets](lua-embedded.md).
- `vectis.http`: generic downstream HTTP and file transfer helpers, documented
  in [Lua HTTP](lua-http.md).
- `vectis.kore`: Kore runtime metadata and constants for Lua code that needs
  to inspect the embedded runtime contract, documented in
  [Lua Kore](lua-kore.md).
- `vectis.lockd`: Vectis lockd client/workflow helpers, documented in
  [Lua lockd](lua-lockd.md).
- `vectis.log`: logging defaults and structured Vectis error fields over pslog,
  documented in [Lua logging](lua-log.md).
- `vectis.mailbox`: bounded in-process service handoff and owner-state Lua
  pumping, documented in [Lua mailbox](lua-mailbox.md).
- `vectis.mqtt`: MQTT publish helper over curl, documented in
  [Lua MQTT](lua-mqtt.md).
- `vectis.rest`: buffered JSON REST route and client helpers, documented in
  [Lua REST](lua-rest.md).
- `vectis.server`: Kore-backed server, route, OpenAPI, and service helpers,
  documented in [Lua server](lua-server.md).
- `vectis.status`: status/error-source constants and Lua error decoration,
  documented in [Lua status](lua-status.md).
- `vectis.smtp`: SMTP send helper over curl, documented in
  [Lua SMTP](lua-smtp.md).
- `vectis.ssh`: SSH/SFTP/SCP helpers, documented in [Lua SSH](lua-ssh.md).
- `vectis.terminal`: Markdown rendering and bounded line editor helpers,
  documented in [Lua terminal](lua-terminal.md).
- `vectis.webdav`: WebDAV client helpers, documented in
  [Lua WebDAV](lua-webdav.md).
- `vectis.xml`: XML parse helpers, documented in [Lua XML](lua-xml.md).

Workflow tables including `vectis.auth`, `vectis.audio_worker`,
`vectis.cert`, `vectis.cai_worker`, `vectis.curl_worker`, `vectis.embedded`,
`vectis.kore`, `vectis.server`, `vectis.ssh`, and `vectis.sus_worker` are preloaded modules inside the embedded
binary. `vectis.auth` re-exports the C-owned `vectis.auth.core` facade and adds Lua DX helpers. `require("vectis").auth` and
`require("vectis.auth")` return the same table, and the same identity rule
applies to the other workflow modules exposed through the top-level namespace.
In ordinary Lua environments, the pure Lua top-level module attaches the
workflow and dependency modules that are available through `package.path`,
`package.cpath`, or `package.preload`, leaving unavailable embedded-only modules
unset instead of fabricating C behavior.

## LuaRocks

`make lua-rock` renders a development rockspec and installs the pure Lua Vectis
helper modules under `build/luarocks`. `make lua-test` verifies both the
embedded runner preloads and the repo-local installed rock with Lua 5.5.
Release builds use `make release-lua-artifacts` to stage the standalone Lua
source archive, render the release rockspec, create the source rock, and
validate that none of those artifacts contain live-worktree or home-directory
paths.

The `vectis` rock does not bundle dependency-native C modules such as
`lockdc`, `lonejson`, `pslog`, `curl.core`, `openssl`, `opcua`, `audio`, or
`sus`. Applications running outside the `vectis` binary must install or preload
those dependency modules separately when they use helpers that require them.

## Documentation Contract

When a bundled dependency becomes useful from Lua app code, add it to this
document and to the `dep:*` section of the coverage matrix. When a Vectis C SDK
workflow gets a Lua helper, add it to this document and to the `workflow:*`
section of the coverage matrix. If the helper is intended for packed
self-contained applications, it also needs packed execution coverage before it
can be considered release-ready.
