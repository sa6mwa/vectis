#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_root=${VECTIS_LUAROCKS_BUILD_ROOT:-"$repo_root/build/luarocks"}
tree=${VECTIS_LUAROCKS_TREE:-"$build_root/tree"}
luarocks=${LUAROCKS:-luarocks}

"$script_dir/build_lua_rock.sh" >/dev/null

lua_bin=
for candidate in "${LUA:-}" lua5.5 lua55 lua; do
  [ -n "$candidate" ] || continue
  if command -v "$candidate" >/dev/null 2>&1; then
    if "$candidate" -e 'assert(_VERSION == "Lua 5.5")' >/dev/null 2>&1; then
      lua_bin=$candidate
      break
    fi
  fi
done

[ -n "$lua_bin" ] || {
  printf '%s\n' "Lua 5.5 is required for make lua-test rock smoke" >&2
  exit 2
}

command -v "$luarocks" >/dev/null 2>&1 || {
  printf '%s\n' "luarocks is required for make lua-test rock smoke" >&2
  exit 2
}

eval "$("$luarocks" path --tree "$tree")"

smoke="$build_root/vectis-rock-smoke.lua"
cat >"$smoke" <<'LUA'
local function stub(name, module)
  package.loaded[name] = module or { _NAME = name }
end

local last_curl_opts

for _, name in ipairs({
  "lockdc", "lonejson", "pslog", "lql", "cai", "libmdf", "softline",
  "opcua", "openssl", "zlib", "audio", "sus",
}) do
  stub(name)
end
stub("lonejson", {
  encode_json = function(value)
    if type(value) == "table" and value.ok == false then
      return '{"ok":false}'
    end
    return '{"ok":true}'
  end,
  encode_value = function()
    return '{"encoded":true}'
  end,
  decode_json = function()
    return { decoded = true }
  end,
  decode_value = function()
    return { decoded = true }
  end,
})
stub("lockdc", {
  json_null = {},
  encode_json = function()
    return '{"lockd":true}'
  end,
  decode_json = function()
    return { lockd = true }
  end,
})
stub("pslog", {
  new_json = function()
    local logger = {}
    function logger:with()
      return self
    end
    function logger:error()
      return true
    end
    return logger
  end,
  from_env = function()
    return package.loaded.pslog.new_json()
  end,
})
stub("libmdf", {
  render = function(markdown)
    return "rendered:" .. markdown
  end,
  render_stream = function(reader, writer)
    local chunk = reader()
    if chunk then
      writer("rendered:" .. chunk)
    end
    return true
  end,
})
stub("softline", {
  new = function(opts)
    return { opts = opts }
  end,
})
stub("curl.core", {
  version = function()
    return "curl-stub"
  end,
  perform = function(opts)
    last_curl_opts = opts
    return {
      ok = true,
      status = 200,
      body = '{"ok":true}',
    }
  end,
})
stub("vectis.dsv.core", {
  parse_json = function()
    return '[["a"]]'
  end,
  parse_typed = function()
    return { typed = true }
  end,
  parse_spill = function()
    return { path = "spill.json" }
  end,
  each = function(opts)
    opts.on_row(1, { name = "row" })
    return true
  end,
  to_string = function()
    return "a,b\n"
  end,
})
stub("vectis.xml.core", {
  parse = function()
    return { root = "ok" }
  end,
  parse_record = function()
    return { record = true }
  end,
})
stub("vectis.auth.core", {
  BASIC = 1,
  provider_native = function(opts)
    return {
      kind = "native",
      credentials_path = opts.credentials_path,
      state_path = opts.state_path,
      purpose = opts.purpose,
      realm = opts.realm,
    }
  end,
  webdav_key = function(opts)
    return {
      client_id = opts.username,
      client_secret = opts.password,
    }
  end,
  basic_authorization = function(key)
    return "Basic " .. key.client_id .. ":" .. key.client_secret
  end,
})

local expected_modules = {
  "curl",
  "vectis",
  "vectis.auth",
  "vectis.dsv",
  "vectis.http",
  "vectis.lockd",
  "vectis.log",
  "vectis.mqtt",
  "vectis.rest",
  "vectis.smtp",
  "vectis.status",
  "vectis.terminal",
  "vectis.version",
  "vectis.webdav",
  "vectis.xml",
}
local loaded = {}
for _, name in ipairs(expected_modules) do
  local module = require(name)
  assert(type(module) == "table" or type(module) == "string", name)
  loaded[name] = module
end

local curl = loaded.curl
local vectis = require("vectis")
assert(type(vectis) == "table")
assert(vectis.version ~= nil)
assert(type(vectis.libs) == "table")
assert(vectis.libs.lockdc == package.loaded.lockdc)
assert(vectis.libs.sus == package.loaded.sus)
assert(vectis.status.OK == vectis.OK)
assert(vectis.status_string(vectis.OK) == "ok")
assert(curl.version() == "curl-stub")
local curl_json = curl.json({
  url = "http://example.test",
  json = { ok = true },
})
assert(curl_json.ok == true)
assert(curl_json.json.decoded == true)
local http = loaded["vectis.http"]
assert(type(http.form_encode) == "function")
assert(http.form_encode({ space = "a b", multi = { "x", "y" } }) ==
       "multi=x&multi=y&space=a%20b")
local http_result = http.get("http://example.test")
assert(http_result.ok == true)
assert(last_curl_opts.protocols == "http,https")
assert(vectis.auth == require("vectis.auth"))
assert(vectis.auth.core == require("vectis.auth.core"))
local flow = vectis.auth.browser_flow({
  credentials_path = "credentials.json",
  state_path = "state.json",
  realm = "rock",
  purpose = "webdav",
})
local provider = assert(flow:provider())
assert(provider.kind == "native")
assert(provider.credentials_path == "credentials.json")
assert(provider.state_path == "state.json")
assert(provider.purpose == "webdav")
assert(provider.realm == "rock")
local authorization = assert(flow:webdav_authorization({
  username = "rock-user",
  password = "rock-password",
}))
assert(authorization == "Basic rock-user:rock-password")
local dsv = loaded["vectis.dsv"]
assert(dsv.to_string({ rows = { { "a", "b" } } }) == "a,b\n")
assert(type(dsv.parse("a,b\n")) == "table")
local xml = loaded["vectis.xml"]
assert(xml.parse("<root/>").root == "ok")
assert(xml.parse_record("<root/>").record == true)
local webdav = loaded["vectis.webdav"]
local propfind = webdav.propfind("http://example.test/dav")
assert(propfind.ok == true)
assert(last_curl_opts.method == "PROPFIND")
assert(last_curl_opts.headers.Depth == "1")
local mqtt = loaded["vectis.mqtt"]
assert(mqtt.publish({
  url = "mqtt://broker/topic",
  payload = "hello",
}).ok == true)
assert(last_curl_opts.protocols == "mqtt")
assert(last_curl_opts.upload == true)
local smtp = loaded["vectis.smtp"]
assert(smtp.send({
  url = "smtp://mail",
  from = "sender@example.test",
  to = "user@example.test",
  message = "Subject: test\r\n\r\nbody\r\n",
}).ok == true)
assert(last_curl_opts.protocols == "smtp,smtps")
assert(last_curl_opts.smtp.mail_from == "sender@example.test")
local rest = loaded["vectis.rest"]
local rest_response = assert(rest.json({ ok = true }))
assert(rest_response.content_type == rest.JSON_CONTENT_TYPE)
local rest_client = rest.client({ base_url = "http://example.test" })
assert(rest_client.get("/status").ok == true)
local lockd = loaded["vectis.lockd"]
assert(lockd.native == package.loaded.lockdc)
assert(lockd.config({ namespace = "docs" }).default_namespace == "docs")
assert(lockd.encode_json({}) == '{"lockd":true}')
local log = loaded["vectis.log"]
assert(log.native == package.loaded.pslog)
assert(type(log.error_fields({ message = "nope" })) == "table")
assert(assert(log.new({ fields = { app = "rock" } })) ~= nil)
local terminal = loaded["vectis.terminal"]
assert(terminal.markdown("# hi") == "rendered:# hi")
assert(terminal.editor({ prompt = "> " }).opts.line_max_len == 4096)
print("vectis Lua rock ok")
LUA

"$lua_bin" "$smoke"
