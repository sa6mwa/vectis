# Vectis Lua Surface

The `vectis` executable embeds the cpkt Lua runtime and preloads both raw
dependency facades and Vectis-owned workflow helpers. Raw modules should stay
thin over their C implementation. Vectis-owned modules should provide the
service/application DX where a workflow crosses Vectis concepts such as Kore
routes, packed assets, auth, lockd consumers, certificate workflows, or
file-backed transfers.

For release tracking, see the [Lua coverage matrix](lua-coverage-matrix.md).
For shared naming, error, ownership, and payload-shape rules, see the
[Lua facade conventions](lua-conventions.md).
For SUS/audio callback and ownership details, see the
[SUS and audio contract](lua-sus-audio-contract.md).

## Raw Dependency Modules

These modules are intended to expose upstream or bundled C library behavior
with minimal Vectis opinion:

- `lockdc`: upstream liblockdc Lua module.
- `lonejson`: upstream LoneJSON Lua module.
- `pslog`: upstream libpslog Lua module.
- `lql`: upstream liblql Lua module.
- `cai`: upstream CAI Lua module.
- `libmdf`: upstream libmdf Lua module.
- `softline`: upstream softline Lua module.
- `curl`: generic libcurl facade, documented in [Lua curl](lua-curl.md).
- `openssl`: OpenSSL facade, documented in [Lua OpenSSL](lua-openssl.md).
- `zlib`: zlib/gzip compression facade, documented in
  [Lua zlib](lua-zlib.md).
- `opcua`: cpkt OPC UA client/foundation facade.
- `audio`: cpkt audio/miniaudio facade, documented in
  [Lua audio](lua-audio.md).
- `sus`: cpkt SUS/whisper facade, documented in [Lua SUS](lua-sus.md).

The C SDK artifacts intentionally do not ship the embedded Lua runtime, Lua
source tree, or Lua package-manager state. The Lua surface is a product surface
of the `vectis` executable.

## Vectis Workflow Modules

These modules are Vectis-owned helpers over one or more raw modules or C SDK
workflows:

- `vectis`: top-level runtime helpers and module namespace.
- `vectis.auth`: native auth, TOTP/QR, email token, OAuth2/OIDC, and WebDAV key
  helpers, documented in [Lua auth](lua-auth.md).
- `vectis.cert`: certificate/key/CSR/CA workflows, documented in
  [Lua certificates](lua-certs.md).
- `vectis.dsv`: CSV/TSV/DSV parsing and serialization, documented in
  [Lua DSV](lua-dsv.md).
- `vectis.http`: generic downstream HTTP and file transfer helpers, documented
  in [Lua HTTP](lua-http.md).
- `vectis.lockd`: Vectis lockd client/workflow helpers, documented in
  [Lua lockd](lua-lockd.md).
- `vectis.mqtt`: MQTT publish helper over curl, documented in
  [Lua MQTT](lua-mqtt.md).
- `vectis.rest`: buffered JSON REST route and client helpers, documented in
  [Lua REST](lua-rest.md).
- `vectis.server`: Kore-backed server helpers, documented in
  [Lua server](lua-server.md).
- `vectis.status`: status/error-source constants and Lua error decoration,
  documented in [Lua status](lua-status.md).
- `vectis.smtp`: SMTP send helper over curl, documented in
  [Lua SMTP](lua-smtp.md).
- `vectis.ssh`: SSH/SFTP/SCP helpers, documented in [Lua SSH](lua-ssh.md).
- `vectis.webdav`: WebDAV client helpers, documented in
  [Lua WebDAV](lua-webdav.md).
- `vectis.xml`: XML parse helpers, documented in [Lua XML](lua-xml.md).

## Documentation Contract

When a bundled dependency becomes useful from Lua app code, add it to this
document and to the `dep:*` section of the coverage matrix. When a Vectis C SDK
workflow gets a Lua helper, add it to this document and to the `workflow:*`
section of the coverage matrix. If the helper is intended for packed
self-contained applications, it also needs packed execution coverage before it
can be considered release-ready.
