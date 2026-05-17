#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
preset=${1:-deps-host-debug}

downloads_dir="$repo_root/.cache/downloads"
deps_root=
target_id=
system_sha256=
lockdc_sha256=
lonejson_sha256=
pslog_sha256=
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
    target_id="x86_64-linux-gnu"
    system_sha256="4e6c4ca07c0647a05923b4a56ef12d440a1d1b53465224e30d990fc18777aa4e"
    lockdc_sha256="0fb8b96297b964e5addbcdc2d552b7749f072e8871b8144bb9219d5d9e0e0ff5"
    lonejson_sha256="f3998e52bfc6c13dba558f736cfd7593ff0571cefb831f806d9605b981275d8d"
    pslog_sha256="91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8"
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
    target_id="x86_64-linux-gnu"
    system_sha256="4e6c4ca07c0647a05923b4a56ef12d440a1d1b53465224e30d990fc18777aa4e"
    lockdc_sha256="0fb8b96297b964e5addbcdc2d552b7749f072e8871b8144bb9219d5d9e0e0ff5"
    lonejson_sha256="f3998e52bfc6c13dba558f736cfd7593ff0571cefb831f806d9605b981275d8d"
    pslog_sha256="91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    target_id="x86_64-linux-musl"
    system_sha256="d44f70558b961125c96d356d27ce83fc7d50c9cc650a335c2016c8d3778d98aa"
    lockdc_sha256="28aa3e786ac1763b78d264a2ddeb4b75a5704ae6e3eca2690767af53c7cc551e"
    lonejson_sha256="e7bf4533a31cb366b5e553b81758997c4d8e1630a326810358310b4bcb8112cb"
    pslog_sha256="b628d32f9207e5102c9a8ae3f7ad32ce36e61178c7db67e6aa4548eb9cae567d"
    target_cc="${CC:-x86_64-linux-musl-gcc}"
    target_ar="${AR:-x86_64-linux-musl-ar}"
    target_ranlib="${RANLIB:-x86_64-linux-musl-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    target_id="aarch64-linux-gnu"
    system_sha256="c20969872de3087f984e8bca3e01fa98e495a3581940e426d07ebed014cf8190"
    lockdc_sha256="03ff83c364c3c7c75c3d9ccb7ab2623cccacbf93d4292e4e7c59d26e0c268f82"
    lonejson_sha256="073160587fa2151eefdc13238eb71677f1dc7740c89a5ad42f86b2a4f25b332f"
    pslog_sha256="d936ae9416f539c4f40aeaa023b9147cbd568bc87b7a3c3b091adfd217d935bb"
    target_cc="${CC:-aarch64-linux-gnu-gcc}"
    target_ar="${AR:-aarch64-linux-gnu-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-gnu-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    target_id="aarch64-linux-musl"
    system_sha256="8ff3cc3c457dc66918470beaea01744bc38c342a87c20c6b072761c56c858e19"
    lockdc_sha256="8ebaaba73eac9e2dd835060a8cab13034e00459af554f800be8341445c2905de"
    lonejson_sha256="0a4669d8b2644132f1bf8288e14893e866d39be3adc37b652651d714deae90fa"
    pslog_sha256="638725174cf39f3c5337fc6f118bc88c2a41d385a01be98170ff4bef3d57fcae"
    target_cc="${CC:-aarch64-linux-musl-gcc}"
    target_ar="${AR:-aarch64-linux-musl-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-musl-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    target_id="armhf-linux-gnu"
    system_sha256="26787953d690b0f01a11538e8692f68f9c746b8e97a9baf47ac15241d9a947fc"
    lockdc_sha256="fb60639a522b75e5e462d7796d4b62bff00bf85fb4a241764bad732a070204d9"
    lonejson_sha256="3a2ba02d7054f663c588b089d67ef23f99cdc9748730084abbece08b5acdc023"
    pslog_sha256="bc8530a3773666deb6d551263c7dd59a64c92629fa56d1e89c278d637472f2dc"
    target_cc="${CC:-arm-linux-gnueabihf-gcc}"
    target_ar="${AR:-arm-linux-gnueabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-gnueabihf-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    target_id="armhf-linux-musl"
    system_sha256="f0172a6ff928111cfaeb503b01b48b3cdd2c05a04d54047630180ee79f65af31"
    lockdc_sha256="14eac9a63d349342110d91f662d915bd28c0b3980390d3435d893e422eb1c74d"
    lonejson_sha256="80e4608295a6c9ba70edcc418a6839c6904d5fe2bdde2187cca23766d2435f84"
    pslog_sha256="503d2bd882c053dc8f34dbfe718a328303a4973789bbb8fb37261e4822b3babe"
    target_cc="${CC:-arm-linux-musleabihf-gcc}"
    target_ar="${AR:-arm-linux-musleabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-musleabihf-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    target_lua_syscflags="-DLUA_USE_POSIX"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    target_id="arm64-apple-darwin"
    system_sha256="dba4424de9566c2418162f62e5e90c45b40266c6e750b5096d4a251bf96d8e9a"
    lockdc_sha256="c7d313ac8dc38f1a98281767f240ef8bb29a63baa65c9b528f6d9893c2d21645"
    lonejson_sha256="4e8730906de7159c54a9eca3f6107846543592e20d97b3c0b9dae1d3cdfc8d1f"
    pslog_sha256="f8f4e18810ecad7278eb341fbfe7e3f9d85eb654891c4d08149f425f3a4c9b3d"
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

system_version="0.1.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$downloads_dir/$system_archive"
lockdc_version="0.9.0"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$downloads_dir/$lockdc_archive"
lonejson_version="0.16.0"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$downloads_dir/$lonejson_archive"
pslog_version="0.4.1"
pslog_archive="libpslog-${pslog_version}-${target_id}.tar.gz"
pslog_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_archive}"
pslog_download="$downloads_dir/$pslog_archive"
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

download_if_missing "$system_url" "$system_download"
download_if_missing "$lockdc_url" "$lockdc_download"
download_if_missing "$lonejson_url" "$lonejson_download"
download_if_missing "$pslog_url" "$pslog_download"
download_if_missing "$pid0_url" "$pid0_download"
download_if_missing "$lua_url" "$lua_download"
download_if_missing "$libxml2_url" "$libxml2_download"

actual_system_sha256=$(sha256sum "$system_download" | awk '{print $1}')
if [ "$actual_system_sha256" != "$system_sha256" ]; then
  echo "checksum mismatch for $system_archive" >&2
  echo "expected $system_sha256" >&2
  echo "actual   $actual_system_sha256" >&2
  exit 1
fi
actual_sha256=$(sha256sum "$lockdc_download" | awk '{print $1}')
if [ "$actual_sha256" != "$lockdc_sha256" ]; then
  echo "checksum mismatch for $lockdc_archive" >&2
  echo "expected $lockdc_sha256" >&2
  echo "actual   $actual_sha256" >&2
  exit 1
fi
actual_lonejson_sha256=$(sha256sum "$lonejson_download" | awk '{print $1}')
if [ "$actual_lonejson_sha256" != "$lonejson_sha256" ]; then
  echo "checksum mismatch for $lonejson_archive" >&2
  echo "expected $lonejson_sha256" >&2
  echo "actual   $actual_lonejson_sha256" >&2
  exit 1
fi
actual_pslog_sha256=$(sha256sum "$pslog_download" | awk '{print $1}')
if [ "$actual_pslog_sha256" != "$pslog_sha256" ]; then
  echo "checksum mismatch for $pslog_archive" >&2
  echo "expected $pslog_sha256" >&2
  echo "actual   $actual_pslog_sha256" >&2
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
tar -xzf "$system_download" -C "$deps_root" --strip-components 1
tar -xzf "$lockdc_download" -C "$deps_root" --strip-components 1
tar -xzf "$lonejson_download" -C "$deps_root" --strip-components 1
tar -xzf "$pslog_download" -C "$deps_root" --strip-components 1
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
system_archive=$system_archive
system_version=$system_version
system_sha256=$system_sha256
liblockdc_archive=$lockdc_archive
liblockdc_version=$lockdc_version
liblockdc_sha256=$lockdc_sha256
lonejson_archive=$lonejson_archive
lonejson_version=$lonejson_version
lonejson_sha256=$lonejson_sha256
pslog_archive=$pslog_archive
pslog_version=$pslog_version
pslog_sha256=$pslog_sha256
pslog_source=libpslog-release
lonejson_source=lonejson-release
curl_source=c.pkt.systems
openssl_source=c.pkt.systems
libssh2_source=c.pkt.systems
nghttp2_source=c.pkt.systems
zlib_source=c.pkt.systems
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
