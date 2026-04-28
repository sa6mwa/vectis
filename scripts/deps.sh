#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
preset=${1:-deps-host-debug}

downloads_dir="$repo_root/.cache/downloads"
deps_root=
lockdc_arch=
lockdc_sha256=
target_cc=
target_ar=
target_ranlib=
target_lua_syscflags=
target_lua_mycflags="-fPIC"

case "$preset" in
  deps-host-debug)
    deps_root="$repo_root/.cache/deps/host-debug"
    lockdc_arch="x86_64"
    lockdc_platform="linux-gnu"
    lockdc_sha256="61aa9200899a5104e49e2cd9d6710c8c3984827d5e7b2c4c0576c64545fc88d5"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    target_lua_mycflags="-fPIC -DLUA_USE_APICHECK"
    ;;
  deps-x86_64-linux-gnu)
    deps_root="$repo_root/.cache/deps/x86_64-linux-gnu"
    lockdc_arch="x86_64"
    lockdc_platform="linux-gnu"
    lockdc_sha256="61aa9200899a5104e49e2cd9d6710c8c3984827d5e7b2c4c0576c64545fc88d5"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    lockdc_arch="x86_64"
    lockdc_platform="linux-musl"
    lockdc_sha256="906555517b3f48c57b44acf8c0be7f0f700a5fcdac2161ae18afc3c646ca8190"
    target_cc="${CC:-x86_64-linux-musl-gcc}"
    target_ar="${AR:-x86_64-linux-musl-ar}"
    target_ranlib="${RANLIB:-x86_64-linux-musl-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    lockdc_arch="aarch64"
    lockdc_platform="linux-gnu"
    lockdc_sha256="c2bdb30456e5f182eff1eb796337e7b78ee24f42fad5ba69378b3312b4948e2f"
    target_cc="${CC:-aarch64-linux-gnu-gcc}"
    target_ar="${AR:-aarch64-linux-gnu-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-gnu-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    lockdc_arch="aarch64"
    lockdc_platform="linux-musl"
    lockdc_sha256="b9ae97df09b59465b01304156fbe651b04cc4e373c9a2f61d9e304ed8ec4348a"
    target_cc="${CC:-aarch64-linux-musl-gcc}"
    target_ar="${AR:-aarch64-linux-musl-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-musl-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    lockdc_arch="armhf"
    lockdc_platform="linux-gnu"
    lockdc_sha256="8212dd2d7396c29f863a69bf6f41d42faef5acccf63ca65a14252365fb022858"
    target_cc="${CC:-arm-linux-gnueabihf-gcc}"
    target_ar="${AR:-arm-linux-gnueabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-gnueabihf-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    lockdc_arch="armhf"
    lockdc_platform="linux-musl"
    lockdc_sha256="43e8c04285b221d3d206dafd7ab2d0747eabdab01c71b29469a4102eda984129"
    target_cc="${CC:-arm-linux-musleabihf-gcc}"
    target_ar="${AR:-arm-linux-musleabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-musleabihf-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    lockdc_arch="arm64"
    lockdc_platform="apple-darwin"
    lockdc_sha256="7c2535b2683f90ae1d5bcff11edb3d51fa8211ae4205570bdbee4a8eec38df68"
    if [ -n "${OSXCROSS_ROOT:-}" ]; then
      osxcross_root=$OSXCROSS_ROOT
    else
      osxcross_root="${HOME:-}/.local/cross/osxcross"
    fi
    osxcross_host="${VECTIS_OSXCROSS_HOST:-arm64-apple-darwin25}"
    target_cc="${CC:-$osxcross_root/bin/$osxcross_host-clang}"
    target_ar="${AR:-$osxcross_root/bin/$osxcross_host-ar}"
    target_ranlib="${RANLIB:-$osxcross_root/bin/$osxcross_host-ranlib}"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  *)
    echo "usage: scripts/deps.sh [deps-host-debug|deps-x86_64-linux-gnu|deps-x86_64-linux-musl|deps-aarch64-linux-gnu|deps-aarch64-linux-musl|deps-armhf-linux-gnu|deps-armhf-linux-musl|deps-arm64-apple-darwin]" >&2
    exit 2
    ;;
esac

mkdir -p "$downloads_dir" "$deps_root/include" "$deps_root/lib"

lockdc_version="0.3.0"
lockdc_archive="liblockdc-${lockdc_version}-${lockdc_arch}-${lockdc_platform}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$downloads_dir/$lockdc_archive"
pid0_version="0.3.0"
pid0_header="libpid0-${pid0_version}.h"
pid0_header_gz="${pid0_header}.gz"
pid0_url="https://github.com/sa6mwa/libpid0/releases/download/v${pid0_version}/${pid0_header_gz}"
pid0_download="$downloads_dir/$pid0_header_gz"
pid0_sha256="29591e058ab5ad9d0bbe0cd4c0d783ace2dee4e2ae6c590a0ba1d9f146f025f9"
lua_version="5.5.0"
lua_archive="lua-${lua_version}.tar.gz"
lua_url="https://www.lua.org/ftp/${lua_archive}"
lua_download="$downloads_dir/$lua_archive"
lua_sha256="57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d"
lua_build_root="$repo_root/.cache/deps-build/$preset"
lua_build_dir="$lua_build_root/lua-${lua_version}"
manifest_path="$deps_root/manifest.txt"

download_if_missing() {
  url=$1
  out=$2
  if [ ! -f "$out" ]; then
    curl -L --fail --retry 3 --output "$out" "$url"
  fi
}

download_if_missing "$lockdc_url" "$lockdc_download"
download_if_missing "$pid0_url" "$pid0_download"
download_if_missing "$lua_url" "$lua_download"

actual_sha256=$(sha256sum "$lockdc_download" | awk '{print $1}')
if [ "$actual_sha256" != "$lockdc_sha256" ]; then
  echo "checksum mismatch for $lockdc_archive" >&2
  echo "expected $lockdc_sha256" >&2
  echo "actual   $actual_sha256" >&2
  exit 1
fi
actual_pid0_sha256=$(sha256sum "$pid0_download" | awk '{print $1}')
if [ "$actual_pid0_sha256" != "$pid0_sha256" ]; then
  echo "checksum mismatch for $pid0_header_gz" >&2
  echo "expected $pid0_sha256" >&2
  echo "actual   $actual_pid0_sha256" >&2
  exit 1
fi
actual_lua_sha256=$(sha256sum "$lua_download" | awk '{print $1}')
if [ "$actual_lua_sha256" != "$lua_sha256" ]; then
  echo "checksum mismatch for $lua_archive" >&2
  echo "expected $lua_sha256" >&2
  echo "actual   $actual_lua_sha256" >&2
  exit 1
fi

rm -rf "$deps_root/include" "$deps_root/lib" "$deps_root/share"
mkdir -p "$deps_root"
tar -xzf "$lockdc_download" -C "$deps_root" --strip-components 1
gzip -dc "$pid0_download" > "$deps_root/include/$pid0_header"
rm -rf "$lua_build_dir"
mkdir -p "$lua_build_root"
tar -xzf "$lua_download" -C "$lua_build_root"
MAKEFLAGS= make -C "$lua_build_dir/src" a \
  CC="$target_cc -std=gnu99" \
  AR="$target_ar rc" \
  RANLIB="$target_ranlib" \
  SYSCFLAGS="$target_lua_syscflags" \
  MYCFLAGS="$target_lua_mycflags" \
  SYSLIBS=""
cp "$lua_build_dir/src/liblua.a" "$deps_root/lib/liblua.a"
cp "$lua_build_dir/src/lua.h" "$deps_root/include/lua.h"
cp "$lua_build_dir/src/luaconf.h" "$deps_root/include/luaconf.h"
cp "$lua_build_dir/src/lualib.h" "$deps_root/include/lualib.h"
cp "$lua_build_dir/src/lauxlib.h" "$deps_root/include/lauxlib.h"
cp "$lua_build_dir/src/lua.hpp" "$deps_root/include/lua.hpp"

cat > "$manifest_path" <<EOF
preset=$preset
liblockdc_archive=$lockdc_archive
liblockdc_version=$lockdc_version
liblockdc_sha256=$lockdc_sha256
pslog_source=liblockdc-bundle
lonejson_source=liblockdc-bundle
curl_source=liblockdc-bundle
openssl_source=liblockdc-bundle
libssh2_source=liblockdc-bundle
libpid0_version=$pid0_version
libpid0_header=$pid0_header
libpid0_sha256=$pid0_sha256
lua_version=$lua_version
lua_archive=$lua_archive
lua_sha256=$lua_sha256
lua_source=lua.org
lua_linkage=static
lua_runtime_module_loading=static-package-preload
EOF
