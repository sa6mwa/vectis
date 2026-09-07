#!/usr/bin/env bash
set -euo pipefail

skill_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
bootlin_resolver="$skill_dir/scripts/cpkt-toolchains.sh"
scratch_root="$skill_dir/build/cpkt-toolchain-tests"
mkdir -p "$scratch_root"
cache=$(mktemp -d "$scratch_root/cache.XXXXXX")
trap 'rm -rf "$cache"' EXIT HUP INT TERM

fail() {
  printf 'test-cpkt-toolchain-resolvers: %s\n' "$*" >&2
  exit 1
}

grep -Fq 'with_cache_lock "$(cache_root)/locks/bootlin-$name.lock" install_bootlin_locked "$target"' "$bootlin_resolver" || fail 'Bootlin root publication is not serialized'
grep -Fq 'if bootlin_ready "$root" "$prefix" "$root/$sysroot_rel"; then' "$bootlin_resolver" || fail 'Bootlin root readiness is not rechecked under the lock'

require_line() {
  local expected=$1 output=$2
  printf '%s\n' "$output" | grep -Fqx "$expected" || fail "missing output: $expected"
}

make_executable() {
  printf '%b\n' "$2" > "$1"
  chmod +x "$1"
}

make_bootlin_collection() {
  local name=$1 prefix=$2 sysroot_rel=$3 root sysroot tool
  root="$cache/roots/$name"
  sysroot="$root/$sysroot_rel"
  mkdir -p "$root/bin" "$sysroot/usr/include" "$sysroot/usr/lib" "$root/runtime"
  : > "$sysroot/usr/include/stdio.h"
  : > "$sysroot/usr/lib/libc.so"
  : > "$root/runtime/libstdc++.a"
  : > "$root/runtime/libgcc.a"
  for tool in gcc ld ar ranlib strip nm objcopy objdump addr2line gdb readelf; do
    make_executable "$root/bin/$prefix-$tool" '#!/bin/sh\nexit 0'
  done
  make_executable "$root/bin/$prefix-g++" "#!/bin/sh\ncase \"\$1\" in\n  -print-file-name=libstdc++.a) printf '%s\\n' '$root/runtime/libstdc++.a' ;;\n  -print-file-name=libgcc.a) printf '%s\\n' '$root/runtime/libgcc.a' ;;\n  *) exit 1 ;;\nesac"
}

for target in x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl; do
  case "$target" in
    x86_64-*) arch=x86-64; prefix=x86_64-linux; triple=x86_64-buildroot-linux ;;
    aarch64-*) arch=aarch64; prefix=aarch64-linux; triple=aarch64-buildroot-linux ;;
    armhf-*) arch=armv7-eabihf; prefix=arm-linux; triple=arm-buildroot-linux ;;
  esac
  case "$target" in
    *-gnu) libc=glibc; abi=gnu ;;
    *-musl) libc=musl; abi=musl ;;
  esac
  if [[ "$arch" == armv7-eabihf ]]; then abi="${abi}eabihf"; fi
  name="$arch--$libc--stable-2026.08-1"
  make_bootlin_collection "$name" "$prefix" "$triple-$abi/sysroot"
  bootlin_description=$(CPKT_TOOLCHAIN_CACHE="$cache" "$bootlin_resolver" discover "$target")
  require_line 'source=bootlin' "$bootlin_description"
  require_line 'status=ready' "$bootlin_description"
  require_line "archive=$name.tar.xz" "$bootlin_description"
  require_line "sysroot=$cache/roots/$name/$triple-$abi/sysroot" "$bootlin_description"
  bootlin_env=$(CPKT_TOOLCHAIN_CACHE="$cache" "$bootlin_resolver" env "$target")
  for tool in cc ld nm; do
    driver=$tool
    if [[ "$tool" == cc ]]; then driver=gcc; fi
    require_line "$tool=$cache/roots/$name/bin/$prefix-$driver" "$bootlin_description"
    printf '%s\n' "$bootlin_env" | grep -Fq "export ${tool^^}=$cache/roots/$name/bin/$prefix-$driver" || fail "Bootlin env did not export $tool for $target"
  done
done

printf 'toolchain resolver tests passed\n'
