#!/usr/bin/env bash
set -eu

target_id=${1:?usage: target_toolchain_available.sh TARGET_ID}

have_tool() {
  tool=$1
  case "$tool" in
    */*) [ -x "$tool" ] ;;
    *) command -v "$tool" >/dev/null 2>&1 ;;
  esac
}

case "$target_id" in
  x86_64-linux-gnu)
    cc_tool=${CC:-cc}
    ar_tool=${AR:-ar}
    ranlib_tool=${RANLIB:-ranlib}
    ;;
  x86_64-linux-musl)
    cc_tool=${CC:-x86_64-linux-musl-gcc}
    ar_tool=${AR:-x86_64-linux-musl-ar}
    ranlib_tool=${RANLIB:-x86_64-linux-musl-ranlib}
    ;;
  aarch64-linux-gnu)
    cc_tool=${CC:-aarch64-linux-gnu-gcc}
    ar_tool=${AR:-aarch64-linux-gnu-ar}
    ranlib_tool=${RANLIB:-aarch64-linux-gnu-ranlib}
    ;;
  aarch64-linux-musl)
    cc_tool=${CC:-aarch64-linux-musl-gcc}
    ar_tool=${AR:-aarch64-linux-musl-ar}
    ranlib_tool=${RANLIB:-aarch64-linux-musl-ranlib}
    ;;
  armhf-linux-gnu)
    cc_tool=${CC:-arm-linux-gnueabihf-gcc}
    ar_tool=${AR:-arm-linux-gnueabihf-ar}
    ranlib_tool=${RANLIB:-arm-linux-gnueabihf-ranlib}
    ;;
  armhf-linux-musl)
    cc_tool=${CC:-arm-linux-musleabihf-gcc}
    ar_tool=${AR:-arm-linux-musleabihf-ar}
    ranlib_tool=${RANLIB:-arm-linux-musleabihf-ranlib}
    ;;
  arm64-apple-darwin)
    exec "$(dirname -- "$0")/osxcross_available.sh"
    ;;
  *)
    echo "unknown target id: $target_id" >&2
    exit 2
    ;;
esac

missing=
for tool in "$cc_tool" "$ar_tool" "$ranlib_tool"; do
  if ! have_tool "$tool"; then
    missing="${missing:+$missing }$tool"
  fi
done

if [ -n "$missing" ]; then
  echo "missing target tools for $target_id: $missing" >&2
  exit 1
fi

probe_dir=${TMPDIR:-/tmp}
probe_c=$probe_dir/vectis-target-probe-$$.c
probe_o=$probe_dir/vectis-target-probe-$$.o
cleanup() {
  rm -f "$probe_c" "$probe_o"
}
trap cleanup EXIT INT TERM

cat >"$probe_c" <<'EOF'
#include <sys/queue.h>
#include <sys/syscall.h>
#ifndef SYS_newfstatat
#error missing SYS_newfstatat
#endif
int main(void) { return SYS_newfstatat == 0; }
EOF

if ! "$cc_tool" -c "$probe_c" -o "$probe_o" >/dev/null 2>&1; then
  echo "target C library for $target_id is missing headers or syscalls required by Kore" >&2
  exit 1
fi
