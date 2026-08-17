#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$repo_root/build/no-kore"
tmp_script="${TMPDIR:-/tmp}/vectis-no-kore-contract.$$"

cleanup() {
  rm -f "$tmp_script"
}
trap cleanup EXIT HUP INT TERM

cmake -S "$repo_root" -B "$build_dir" -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVECTIS_EXTERNAL_ROOT="$repo_root/.cache/deps/host-debug" \
  -DVECTIS_WITH_KORE_RUNTIME=OFF \
  -DVECTIS_BUILD_STATIC=ON \
  -DVECTIS_BUILD_SHARED=OFF \
  -DVECTIS_BUILD_BINARY=ON \
  -DVECTIS_BUILD_TESTS=OFF \
  -DVECTIS_BUILD_FUZZERS=OFF \
  -DVECTIS_INSTALL=OFF

cmake --build "$build_dir"

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
