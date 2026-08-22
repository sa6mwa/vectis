# Lua SMTP Facade

`vectis.smtp` is a small Vectis-owned wrapper over the generic `curl` Lua
facade. It keeps SMTP as a normal curl protocol surface while providing names
that are convenient for app code.

For using this helper from a Lua site form or notification route, see
[Serving a Lua site](lua-site.md).

## API

- `vectis.smtp.send(opts)` sends one message with `curl.perform()`.

Required fields:

- `url`: `smtp://` or `smtps://` endpoint.
- `mail_from` or `from`.
- `rcpt`, `recipients`, or `to`: string or table of recipient addresses.
- `body`, `message`, `body_path`, or `upload_path`.

Optional fields are passed through to `curl.perform()`, including credentials,
TLS verification, proxy, retry, timeout, and protocol allowlist controls.
`protocols` defaults to `smtp,smtps`.

```lua
local smtp = require("vectis.smtp")

local result = smtp.send({
  url = "smtps://mail.example.test:465",
  username = "user",
  password = "secret",
  from = "noreply@example.test",
  to = {"ops@example.test"},
  body_path = "message.eml",
})
assert(result.ok, result.error and result.error.message)
```

Results are normalized like other Vectis protocol helpers. Transport failures
return a result table with `ok = false`, `transport_ok = false`, and a
structured `error` carrying Vectis status/source metadata, `source = "curl"`,
and the CURLcode in `error.dependency_code` when available.
