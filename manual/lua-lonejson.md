# Lua LoneJSON

`require("lonejson")` imports the authoritative installed upstream Lua facade
through an internal `lonejson.upstream` resource. A small Vectis overlay keeps
`lonejson.schema(name, fields)` and root `array_rewrite_string/path`; it does
not fork or replace the dependency's exports. `lonejson.new()` creates a runtime
whose `runtime:schema(...)` supplies ownership for schemas and decoded records.

The full upstream native import supports JSON encode/decode, incremental path
visitors, binary Base64, JWT/JWK/JWKS parsing and validation, OAuth2 bodies and
OIDC helpers. Vectis compiles the JWT/OIDC/OpenSSL provider integration as well
as linking it. JWT parsing is not validation; configure a runtime auth provider
and validate the required claims and signature. Root PKCE helpers are available;
provider/live OIDC execution is covered by the opt-in auth gates, not the
malformed-token tests.

```lua
local json=require("lonejson")
local runtime=json.new()
local schema=runtime:schema("person", {json.field("name",json.string())})
local person=assert(schema:decode('{"name":"Ada"}'))
assert(person.name=="Ada")
assert(json.base64_decode(json.base64_encode("a\0b"))=="a\0b")
```

Visitors deliver borrowed chunks during the callback, using bounded upstream
parser buffers. Callback errors return the dependency error and release retained
callbacks. Spools are explicitly file-backed; table/record decode materializes
values. None of these labels imply a pull payload source for workflow jobs.

`vectis -a source --module lonejson` returns the small overlay;
`--module lonejson.upstream` returns the imported authoritative source. Vectis
source rocks package only Vectis modules and depend on installed upstream
modules. They exclude this embedded-only overlay so an ordinary Lua path cannot
shadow the installed LoneJSON root with an unavailable internal resource.
