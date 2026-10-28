#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
deps_root=${1:-"$repo_root/.cache/deps/host-debug"}
upstream_dir=${2:-"$repo_root/vendor/kore/upstream"}

if [ ! -d "$upstream_dir/.git" ]; then
  echo "vendor/kore/upstream is missing; run 'make vendor-kore' first" >&2
  exit 1
fi

if [ ! -d "$deps_root/cpkt/include" ] || [ ! -d "$deps_root/pslog/lib" ] ||
   [ ! -d "$deps_root/lonejson/lib" ] || [ ! -f "$deps_root/manifest.txt" ]; then
  echo "dependency root '$deps_root' is incomplete; run 'make deps-debug' or pass a valid deps root" >&2
  exit 1
fi

deps_root=$(CDPATH= cd -- "$deps_root" && pwd)
upstream_dir=$(CDPATH= cd -- "$upstream_dir" && pwd)
case "$deps_root" in "$repo_root"/.cache/*|"$repo_root"/build/*) ;;
  *) echo 'Kore dependency prefixes must belong to this checkout' >&2; exit 2;;
esac
case "$upstream_dir" in "$repo_root"/vendor/kore/upstream|"$repo_root"/build/*) ;;
  *) echo 'Kore source must belong to this checkout' >&2; exit 2;;
esac

target=$(sed -n 's/^target_id=//p' "$deps_root/manifest.txt")
local_flags=${LDFLAGS:-}
case "$target:$(uname -s):$(uname -m)" in
  x86_64-linux-gnu:Linux:x86_64|aarch64-linux-gnu:Linux:aarch64|armhf-linux-gnu:Linux:armv7l)
    description=$("$script_dir/cpkt-toolchains.sh" discover "$target")
    compiler=$(printf '%s\n' "$description" | sed -n 's/^cc=//p')
    sysroot=$(printf '%s\n' "$description" | sed -n 's/^sysroot=//p')
    loaders=("$sysroot"/lib/ld-linux*.so.*)
    [[ ${#loaders[@]} == 1 && -f ${loaders[0]} && -x $compiler ]] || {
      echo 'Prepared native Bootlin compiler/runtime is required for the Kore check' >&2; exit 2;
    }
    local_flags+=" -Wl,--dynamic-linker=${loaders[0]} -Wl,--disable-new-dtags -Wl,-rpath,$sysroot/lib:$sysroot/usr/lib:$sysroot/../lib64:$deps_root/cpkt/lib:$deps_root/pslog/lib:$deps_root/lonejson/lib"
    ;;
  arm64-apple-darwin:Darwin:arm64)
    compiler=$(xcrun --find clang)
    local_flags+=" -Wl,-rpath,$deps_root/cpkt/lib -Wl,-rpath,$deps_root/pslog/lib -Wl,-rpath,$deps_root/lonejson/lib"
    ;;
  *) echo 'Standalone Kore verification requires the native GNU/Linux or Apple SDK selection' >&2; exit 2;;
esac
[[ -f "$upstream_dir/src/vectis_pslog_impl.c" ]] || {
  echo 'Kore patches are missing; run bash scripts/vendor-kore.sh apply first' >&2; exit 2;
}

LDFLAGS="$local_flags" make -C "$upstream_dir" CC="$compiler" OPENSSL_PATH="$deps_root/cpkt" \
  PSLOG=1 \
  LONEJSON=1 \
  CURL=1 \
  ACME=1 \
  KORE_BUNDLE_ROOT="$deps_root"
