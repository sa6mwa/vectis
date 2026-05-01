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
target_cmake_system_name=
target_cmake_system_processor=
target_lua_syscflags=
target_lua_mycflags="-fPIC"

case "$preset" in
  deps-host-debug)
    deps_root="$repo_root/.cache/deps/host-debug"
    lockdc_arch="x86_64"
    lockdc_platform="linux-gnu"
    lockdc_sha256="7cef184a1ba9b9e2f23cd35a3b2fed7acef7b774d1565c2cdaec9e916dcb954c"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    target_lua_mycflags="-fPIC -DLUA_USE_APICHECK"
    ;;
  deps-x86_64-linux-gnu)
    deps_root="$repo_root/.cache/deps/x86_64-linux-gnu"
    lockdc_arch="x86_64"
    lockdc_platform="linux-gnu"
    lockdc_sha256="7cef184a1ba9b9e2f23cd35a3b2fed7acef7b774d1565c2cdaec9e916dcb954c"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    lockdc_arch="x86_64"
    lockdc_platform="linux-musl"
    lockdc_sha256="6a58fb967add059308ff5e6ade8cd01e7cb91443acb1c33068d037519c046ad5"
    target_cc="${CC:-x86_64-linux-musl-gcc}"
    target_ar="${AR:-x86_64-linux-musl-ar}"
    target_ranlib="${RANLIB:-x86_64-linux-musl-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    lockdc_arch="aarch64"
    lockdc_platform="linux-gnu"
    lockdc_sha256="2a515e6dcb925c80e03ef15f09606d3552b6139533c2e786d1f03aa99e4ebb16"
    target_cc="${CC:-aarch64-linux-gnu-gcc}"
    target_ar="${AR:-aarch64-linux-gnu-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-gnu-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    lockdc_arch="aarch64"
    lockdc_platform="linux-musl"
    lockdc_sha256="8df41475dc9e580d99c3e3c9b8289d65dd0fdd4295db75db0d236c36782c0bff"
    target_cc="${CC:-aarch64-linux-musl-gcc}"
    target_ar="${AR:-aarch64-linux-musl-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-musl-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    lockdc_arch="armhf"
    lockdc_platform="linux-gnu"
    lockdc_sha256="f0729e0d318236724bbe99b5084a58a6e2756049af8a071a1355951aa2177985"
    target_cc="${CC:-arm-linux-gnueabihf-gcc}"
    target_ar="${AR:-arm-linux-gnueabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-gnueabihf-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    lockdc_arch="armhf"
    lockdc_platform="linux-musl"
    lockdc_sha256="6419768dce53a1a0e3f9054790d8fcbe2792520f2b79a9e2ef98ffa0b1a845b6"
    target_cc="${CC:-arm-linux-musleabihf-gcc}"
    target_ar="${AR:-arm-linux-musleabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-musleabihf-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    lockdc_arch="arm64"
    lockdc_platform="apple-darwin"
    lockdc_sha256="8d591e7687b2bd98408d5122f19ff8c53bc68b4920f4a98ef1f1a66a67ca1042"
    if [ -n "${OSXCROSS_ROOT:-}" ]; then
      osxcross_root=$OSXCROSS_ROOT
    else
      osxcross_root="${HOME:-}/.local/cross/osxcross"
    fi
    osxcross_host="${VECTIS_OSXCROSS_HOST:-arm64-apple-darwin25}"
    target_cc="${CC:-$osxcross_root/bin/$osxcross_host-clang}"
    target_ar="${AR:-$osxcross_root/bin/$osxcross_host-ar}"
    target_ranlib="${RANLIB:-$osxcross_root/bin/$osxcross_host-ranlib}"
    target_cmake_system_name="Darwin"
    target_cmake_system_processor="arm64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  *)
    echo "usage: scripts/deps.sh [deps-host-debug|deps-x86_64-linux-gnu|deps-x86_64-linux-musl|deps-aarch64-linux-gnu|deps-aarch64-linux-musl|deps-armhf-linux-gnu|deps-armhf-linux-musl|deps-arm64-apple-darwin]" >&2
    exit 2
    ;;
esac

mkdir -p "$downloads_dir" "$deps_root/include" "$deps_root/lib"

resolve_tool_path() {
  tool=$1
  case "$tool" in
    */*) printf '%s\n' "$tool" ;;
    *)
      resolved=$(command -v "$tool" 2>/dev/null || true)
      if [ -z "$resolved" ]; then
        echo "required tool not found: $tool" >&2
        exit 1
      fi
      printf '%s\n' "$resolved"
      ;;
  esac
}

target_ar=$(resolve_tool_path "$target_ar")
target_ranlib=$(resolve_tool_path "$target_ranlib")

lockdc_version="0.4.0"
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
libxml2_version="2.15.3"
libxml2_archive="libxml2-${libxml2_version}.tar.gz"
libxml2_url="https://github.com/GNOME/libxml2/archive/refs/tags/v${libxml2_version}.tar.gz"
libxml2_download="$downloads_dir/$libxml2_archive"
libxml2_sha256="5c6060277173270356c3f1c321a640ab629bdabc5e5ba9095b99e00759ba0c39"
libxml2_build_root="$repo_root/.cache/deps-build/$preset/libxml2-${libxml2_version}"
libxml2_source_dir="$libxml2_build_root/src"
libxml2_static_build_dir="$libxml2_build_root/build-static"
libxml2_shared_build_dir="$libxml2_build_root/build-shared"
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
download_if_missing "$libxml2_url" "$libxml2_download"

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
actual_libxml2_sha256=$(sha256sum "$libxml2_download" | awk '{print $1}')
if [ "$actual_libxml2_sha256" != "$libxml2_sha256" ]; then
  echo "checksum mismatch for $libxml2_archive" >&2
  echo "expected $libxml2_sha256" >&2
  echo "actual   $actual_libxml2_sha256" >&2
  exit 1
fi

build_libxml2() {
  build_dir=$1
  shared=$2

  cmake -S "$libxml2_source_dir" -B "$build_dir" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$deps_root" \
    -DCMAKE_C_COMPILER="$target_cc" \
    -DCMAKE_AR="$target_ar" \
    -DCMAKE_RANLIB="$target_ranlib" \
    -DCMAKE_SYSTEM_NAME="$target_cmake_system_name" \
    -DCMAKE_SYSTEM_PROCESSOR="$target_cmake_system_processor" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DBUILD_SHARED_LIBS="$shared" \
    -DLIBXML2_WITH_DOCS=OFF \
    -DLIBXML2_WITH_PROGRAMS=OFF \
    -DLIBXML2_WITH_TESTS=OFF \
    -DLIBXML2_WITH_PYTHON=OFF \
    -DLIBXML2_WITH_ICONV=OFF \
    -DLIBXML2_WITH_ICU=OFF \
    -DLIBXML2_WITH_MODULES=OFF \
    -DLIBXML2_WITH_READLINE=OFF \
    -DLIBXML2_WITH_ZLIB=OFF
  cmake --build "$build_dir"
  cmake --install "$build_dir"
}

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
rm -rf "$libxml2_build_root"
mkdir -p "$libxml2_build_root"
tar -xzf "$libxml2_download" -C "$libxml2_build_root"
mv "$libxml2_build_root/libxml2-$libxml2_version" "$libxml2_source_dir"
build_libxml2 "$libxml2_static_build_dir" OFF
build_libxml2 "$libxml2_shared_build_dir" ON

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
libxml2_version=$libxml2_version
libxml2_archive=$libxml2_archive
libxml2_sha256=$libxml2_sha256
libxml2_source=GNOME
libxml2_iconv=disabled
libxml2_zlib=disabled
libxml2_linkage=static+shared
EOF
