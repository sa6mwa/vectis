#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
work=$(mktemp -d "$repo_root/build/release-elf-contracts.XXXXXX")
trap 'rm -rf "$work"' EXIT
eval "$("$script_dir/cpkt-toolchains.sh" env x86_64-linux-gnu)"
version=9.8.7
mkdir -p "$work/payload/share/fixtures" "$work/dist" "$work/nested"
source_file="$repo_root/tests/helpers/vectis_runtime_probe.c"
binary="$work/payload/share/fixtures/helper"

check() {
  local expected=$1
  tar -C "$work/payload" -czf "$work/nested/inner.tar.gz" .
  tar -C "$work/nested" -cJf "$work/middle.tar.xz" inner.tar.gz
  tar -C "$work" -czf "$work/dist/vectis-$version.tar.gz" middle.tar.xz
  (cd "$work/dist" && sha256sum "vectis-$version.tar.gz" >"vectis-$version-CHECKSUMS")
  if VECTIS_VERSION=$version VECTIS_DIST_DIR="$work/dist" \
      bash "$script_dir/verify_release_privacy.sh" >"$work/result" 2>&1; then
    [ "$expected" = ok ] || { echo "accepted forbidden ELF metadata: $expected" >&2; exit 1; }
  else
    [ "$expected" != ok ] || { cat "$work/result" >&2; exit 1; }
    grep -F "$expected" "$work/result" >/dev/null || { cat "$work/result" >&2; exit 1; }
    grep -F 'inner.tar.gz.expanded/share/fixtures/helper' "$work/result" >/dev/null
  fi
}

# These are never executed: only packaged metadata is under test.
"$CC" -s "$source_file" -o "$binary" -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
check ok
"$CC" -s "$source_file" -o "$binary" -Wl,--dynamic-linker=/opt/bootlin/sysroot/lib/ld-linux-x86-64.so.2
check 'non-system ELF interpreter'
for tag in --enable-new-dtags --disable-new-dtags; do
  # shellcheck disable=SC2016 # The loader, not this shell, expands ORIGIN.
  "$CC" -s "$source_file" -o "$binary" -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    "-Wl,$tag" '-Wl,-rpath,$ORIGIN:/opt/bootlin/sysroot/lib'
  check 'non-relocatable ELF runtime path'
done
"$CC" -s -shared -fPIC "$source_file" -o "$work/libbad.so" -Wl,-soname,/opt/bootlin/libbad.so
"$CC" -s "$source_file" -o "$binary" -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
  -Wl,--no-as-needed "$work/libbad.so"
check 'non-relocatable ELF dependency path'
"$CC" -s -static "$source_file" -o "$binary"
check ok
mkdir "$work/fakebin"
printf '#!/bin/sh\nexit 1\n' >"$work/fakebin/readelf"
chmod +x "$work/fakebin/readelf"
PATH="$work/fakebin:$PATH" check 'cannot inspect ELF header'
echo 'release ELF contracts ok'
