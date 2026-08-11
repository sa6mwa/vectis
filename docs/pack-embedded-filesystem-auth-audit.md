# Pack Embedded Filesystem And Auth Audit

This audit maps the packed Vectis webserver objective to current executable
evidence. It is a completion aid, not a replacement for the full lifecycle
gates.

## Requirement Evidence

| Requirement | Evidence |
| --- | --- |
| Pack Lua plus site assets and templates into a self-contained executable. | `vectis_lua_pack` packages generated Lua scripts with generated site assets, template files, and embedded metadata through `vectis -a pack`. |
| Serve packed assets as a read-only docroot without extraction. | `tests/lua/pack.cmake` registers `server:static_embedded(...)`, fetches `/`, `/assets/app.txt`, validates ETag/cache-control headers, and verifies WebDAV mutations do not alter the embedded read-only response. |
| Extract packed assets to a mutable docroot. | `tests/lua/pack.cmake` exercises `vectis.embedded.extract(...)` with verify, fail-exists, skip-existing, repair, and overwrite policies, including preservation of unrelated user-created files. |
| Serve extracted assets through WebDAV. | `tests/lua/pack.cmake` registers `server:webdav_embedded_site(...)`, performs authenticated GET, PUT, PROPFIND, MKCOL, COPY, MOVE, and DELETE operations, and verifies writes land in the extracted docroot. |
| Keep WebDAV decoupled from auth through an adapter/provider contract. | `include/vectis/auth.h` defines `vectis_auth_provider`, provider request/response records, native provider construction, and callback provider construction. `tests/unit/test_vectis_auth.c` validates allow, required, redirect, and header-aware provider responses through `vectis_webdav_auth_provider(...)`. |
| Support native username/password, TOTP, email-token, and WebDAV app-key flows. | `tests/unit/test_vectis_auth.c`, `tests/lua/smoke.lua`, and `tests/lua/pack.cmake` cover user enrollment, TOTP QR generation, password login, pending login, email-token issue/verify, SMTP delivery, WebDAV key issuance, logout revocation, and factor combinations including email-only, password-only, password+email-token, password+TOTP, and password+TOTP+email-token. |
| Support native OAuth2/OIDC token flows and WebDAV keys linked to token flow state. | `include/vectis/auth.h` exposes OAuth2 client credentials, stored token flow, OIDC authorization, OIDC callback exchange, and OAuth2-linked WebDAV key APIs. `tests/unit/test_vectis_auth.c` and `tests/lua/smoke.lua` cover browser callback exchange, client-credentials requests, stored flow refresh/failure behavior, OAuth2-linked WebDAV key issuance, default revocation on token-flow failure, and explicit key retention when configured. |
| Let developers register C and Lua auth providers. | C provider registration is covered by `vectis_auth_provider_from_callback(...)` and `vectis_auth_provider_from_native_store(...)`; Lua provider registration is covered by `vectis.auth.provider_native(...)`, `vectis.auth.provider_callback(...)`, guarded `server:auth_json(...)`, and packed WebDAV callback-provider scenarios in `tests/lua/pack.cmake`. |
| Run Kore serving, WebDAV, and lockd `startconsumer` service in one process. | `examples/kore/kore_webdav_lockd_consumer_e2e.c` and `tests/lua/pack.cmake` cover same-process WebDAV/API serving plus C-owned liblockdc consumer service registration, message handling, and continued HTTP/WebDAV responsiveness. |
| Preserve Landed migration constraints without embedding Landed assets. | `tests/lua/pack.cmake` generates a generic Acme site, generated templates, private self-signed certificate material, mock SMTP inputs, and mock ACME inputs. It does not copy Landed site assets. Kore JSON and streaming migration constraints are separately guarded by `assert_kore_lonejson_contract()` in `scripts/test_lifecycle_contracts.sh` and the Kore/WebDAV e2e examples. |

## Gate Set

Focused local evidence:

```sh
ctest --preset debug -R '^(vectis_lua_pack|vectis_unit_auth|vectis_unit_totp_qr|vectis_unit_embedded_fs|vectis_unit_webdav)$' --output-on-failure
bash scripts/test_lifecycle_contracts.sh
```

Full deterministic local scenario evidence:

```sh
make test-e2e
```

Live external OIDC/OAuth2 interoperability remains opt-in:

```sh
VECTIS_LIVE_OAUTH2_ENABLE=1 make prerelease-live
```

