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

for _, name in ipairs({
  "lockdc", "lonejson", "pslog", "lql", "cai", "libmdf", "softline",
  "curl", "opcua", "openssl", "zlib", "audio", "sus",
}) do
  stub(name)
end
stub("curl.core")
stub("vectis.dsv.core")
stub("vectis.xml.core")

local vectis = require("vectis")
assert(type(vectis) == "table")
assert(vectis.version ~= nil)
assert(type(vectis.libs) == "table")
assert(vectis.libs.lockdc == package.loaded.lockdc)
assert(vectis.libs.sus == package.loaded.sus)
assert(vectis.status.OK == vectis.OK)
assert(vectis.status_string(vectis.OK) == "ok")
assert(type(require("vectis.http")) == "table")
assert(type(require("vectis.dsv")) == "table")
assert(type(require("vectis.webdav")) == "table")
print("vectis Lua rock ok")
LUA

"$lua_bin" "$smoke"
