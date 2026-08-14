# Lua HTTP Helpers

`require("vectis").http` is a Vectis-owned helper layer over the preloaded
`curl` module. It keeps protocol coverage in libcurl while giving service Lua
code a consistent result shape for API calls and file transfers.

## Entry Points

- `vectis.http.request(opts)` calls `curl.perform(opts)`.
- `vectis.http.get(opts_or_url[, opts])`, `post`, `put`, `patch`, `delete`,
  `head`, and `options` set the HTTP method and return a normalized buffered
  response.
- `vectis.http.request_json(opts)` calls `curl.json(opts)`.
- `vectis.http.get_json(opts_or_url[, opts])`, `post_json`, `put_json`,
  `patch_json`, `delete_json`, and `options_json` set the HTTP method and
  decode JSON bodies.
- `vectis.http.form(opts)` encodes `opts.form` as
  `application/x-www-form-urlencoded`, defaults to `POST`, and sends it as the
  request body.
- `vectis.http.form_encode(table)` returns a deterministic URL-encoded form
  body. Array values encode as repeated keys.
- `vectis.http.multipart(opts)` sends `opts.multipart` or `opts.parts` through
  libcurl's MIME API and defaults to `POST`.
- `vectis.http.stream_json(opts)` calls `curl.stream_json(opts)`.
- `vectis.http.download(opts)` requires `download_path` and streams the response
  body to that file through libcurl's write callback.
- `vectis.http.download_file(url, path[, opts])` is the same file-backed
  download path with URL/path arguments for common app workflows.
- `vectis.http.upload(opts)` accepts `upload_path`, `body_path`, or `body`.
  File paths stream from disk through libcurl's read callback.
- `vectis.http.upload_file(url, path[, opts])` uploads one local file with
  URL/path arguments.
- `vectis.http.sftp_download(opts)` and `vectis.http.sftp_upload(opts)` set the
  default protocol allowlist to `sftp` and use the same file-backed transfer
  paths.
- `vectis.http.sftp_download_file(url, path[, opts])` and
  `vectis.http.sftp_upload_file(url, path[, opts])` are the curl-backed SFTP
  file-transfer presets.
- `vectis.http.client(defaults)` returns a helper object with the same request
  methods and shared defaults for retry, proxy, TLS, client certificates,
  credentials, headers, protocol allowlists, and timeouts.

## Results

Helpers return the underlying curl result with normalized fields:

- `ok`: true only when the transfer succeeded and HTTP status is below 400.
- `transport_ok`: true when libcurl completed the transfer.
- `attempts`: number of curl attempts, including retries.
- `error`: structured table on failure.
- `error_message`: original curl error string when a transport failure occurred.

Structured errors use:

- `kind = "transport"` for libcurl failures.
- `kind = "http_status"` for HTTP status 400 and above.
- Vectis status metadata: `status`, `status_string`, `source`, and
  `source_code`.
- libcurl failures include `code`, `code_name`, `dependency_code`, and
  `attempts` when available.
- HTTP status failures include `http_status`, `body`, `json`, and `attempts`
  when those fields apply.

Retry options are passed through to `curl.perform`, `curl.json`, and
`curl.stream_json`. Streaming JSON responses still reject retry because the
response parser is consumed by the first attempt.

## Forms

```lua
local response = vectis.http.form({
  url = "https://example.test/login",
  form = {
    username = "alice",
    scope = {"read", "write"},
  },
})
```

`form` sets `Content-Type` only when no case-insensitive `content-type` header
is already present.

## Multipart

```lua
local response = vectis.http.multipart({
  url = "https://example.test/upload",
  parts = {
    description = "release bundle",
    artifact = {
      path = "dist/vectis.tar.gz",
      filename = "vectis.tar.gz",
      content_type = "application/gzip",
    },
  },
})
```

`multipart` accepts numeric part tables and string-key shorthand text fields.
Part tables require `name` unless the table is stored under a string key. Text
parts use `value` or `body`. File parts use `path` or `file_path`, plus
optional `filename` and `content_type`.

## Clients

```lua
local api = vectis.http.client({
  protocols = "https",
  timeout_ms = 5000,
  retry = {
    max_attempts = 3,
    conditions = {"transport", "5xx"},
  },
  headers = {
    Authorization = "Bearer " .. token,
  },
  ca_file = "ca.pem",
  client_cert = "client.pem",
  client_key = "client.key",
})

local response = api.get_json("https://api.example.test/status")
```

Client methods shallow-merge per-request options over defaults. Headers merge
separately, so a request can override or add individual headers without
repeating the default header table.

## File Transfers

```lua
local http = require("vectis.http")

local downloaded = assert(http.download_file(
  "https://example.test/report.csv",
  "report.csv",
  {timeout_ms = 5000}
))

local uploaded = assert(http.upload_file(
  "https://example.test/upload/report.csv",
  "report.csv",
  {timeout_ms = 5000}
))
```

`download_file` sets `download_path`; `upload_file` sets `upload_path`. Table
form also accepts `path` or `local_path` as aliases when `download_path` or
`upload_path` is not already present.
