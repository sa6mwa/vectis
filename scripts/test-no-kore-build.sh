#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
build_dir="$repo_root/build/no-kore"
mkdir -p "$build_dir"
tmp_script=$(mktemp "$build_dir/no-kore-contract.XXXXXXXX.lua")
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT

cleanup() {
  rm -f "$tmp_script"
}
trap cleanup EXIT HUP INT TERM

cmake --preset debug -S "$repo_root" -B "$build_dir" \
  -DVECTIS_WITH_KORE_RUNTIME=OFF \
  -DVECTIS_BUILD_TESTS=OFF \
  -DVECTIS_INSTALL=OFF

cmake --build "$build_dir"
python3 "$script_dir/test_runtime_contract.py" "$build_dir"

cat >"$tmp_script" <<'LUA'
local vectis = require("vectis")
local kore = require("vectis.kore")

assert(vectis.kore == kore)
assert(kore.runtime_available == false)
assert(kore.runtime_model == "none")
assert(kore.MAX_WORKER_COUNT == 253)
assert(kore.websocket.TEXT == vectis.websocket.TEXT)
assert(kore.websocket.BINARY == vectis.websocket.BINARY)

print("vectis no-kore lua contract ok")
LUA

"$build_dir/vectis" "$tmp_script"
