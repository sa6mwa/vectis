#!/usr/bin/env bash
set -euo pipefail

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
cai_sha256=
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
    system_sha256="69e699d18374987ba16dfd82640ba1c263b71f0e1daeef8af2a2018a6f1e39ef"
    lockdc_sha256="0f2d52fd08109ffa2324fa7fc7de773ca804c3737d80aa3e03ff16b0c1976708"
    lonejson_sha256="84f0abae33a1b1d91a0f962ad78ecd7be91a4055c4c6a4aa69e0df816f42812c"
    pslog_sha256="91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8"
    cai_sha256="84e401154f7e81707657da3068f0a57b58bf0899c930a296c98ebe46a5619661"
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
    system_sha256="69e699d18374987ba16dfd82640ba1c263b71f0e1daeef8af2a2018a6f1e39ef"
    lockdc_sha256="0f2d52fd08109ffa2324fa7fc7de773ca804c3737d80aa3e03ff16b0c1976708"
    lonejson_sha256="84f0abae33a1b1d91a0f962ad78ecd7be91a4055c4c6a4aa69e0df816f42812c"
    pslog_sha256="91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8"
    cai_sha256="84e401154f7e81707657da3068f0a57b58bf0899c930a296c98ebe46a5619661"
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
    system_sha256="dcb1923af1d7529531f31637476f083070ccf1051ec217012d77a155ff4132d4"
    lockdc_sha256="d5c33f652daeac137febac21d7dadc1156ab02027e8fa2f55e0800782295fe6b"
    lonejson_sha256="326ee1f75e632db9899e05ac02f669b87a437064086242603519fe5197ea654a"
    pslog_sha256="b628d32f9207e5102c9a8ae3f7ad32ce36e61178c7db67e6aa4548eb9cae567d"
    cai_sha256="ef85c84cdf82f2d37d3db3ebe7c97bcb28dc36072285779df993d8ee8a12d959"
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
    system_sha256="54e9ef336a0b092d0e68a84d2026a499c051df3c3e61b7a7dab1f0103d1e082b"
    lockdc_sha256="50163db0335c73d75dfb70578ac03482d2803d73c7e47b527392267854d7fb51"
    lonejson_sha256="74a515fcce5574f82dbae46a6ce3399d6b5d07d249fa9c98d53293b7baf8c35b"
    pslog_sha256="d936ae9416f539c4f40aeaa023b9147cbd568bc87b7a3c3b091adfd217d935bb"
    cai_sha256="4df06b5e31c2f4b98a29031591bd2994aa77d372efc63e702dcffd3ec035968b"
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
    system_sha256="61f7cb43ca78f33a21b5719f84f864ac5ce5cf7d8268eef52a7a01da08582a38"
    lockdc_sha256="5829cbeaa88a78307e1162edabf38c8cc70e97c2845d170b7008c5f97b378496"
    lonejson_sha256="0f2b6048caf74f5c9c02d332cea00bd67e2352764ba1b0d2f65b4266719fcd39"
    pslog_sha256="638725174cf39f3c5337fc6f118bc88c2a41d385a01be98170ff4bef3d57fcae"
    cai_sha256="04808e14b8c55e2d624ba6ac01866b33b61c4a5913be92418966c557baf393e1"
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
    system_sha256="33bfe771f2f2b36562c9e18094f79168d8d687dc7a55393584930ca434547d7d"
    lockdc_sha256="8b945b35fe70effa10f84f61659ea92600320e16f3cec32160a0cc142b17b4a8"
    lonejson_sha256="8adad849699ea6380dc16ee9228f4d9695e9b3d49ce6c68b7f391e3196e9d6ad"
    pslog_sha256="bc8530a3773666deb6d551263c7dd59a64c92629fa56d1e89c278d637472f2dc"
    cai_sha256="0ed692d3b14e86e404f8ac5d9982d732291e67f8b56e3e028db4d55192318854"
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
    system_sha256="7edc2f61e01370da6af96c496ae5bfbec72e43761247dd2c310ac7627923c591"
    lockdc_sha256="3101c505c3dcd12c7ee9af5822b3b0c2c173be134e6b60dc072b49af853f3e2b"
    lonejson_sha256="e62f512dae09e5fe6402e876edaf1f6517159854e049ff161c32ee52df864566"
    pslog_sha256="503d2bd882c053dc8f34dbfe718a328303a4973789bbb8fb37261e4822b3babe"
    cai_sha256="c9c899f6881a977a5d88218505649531cdac200a4c969400d5c1a7e88a529056"
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
    system_sha256="204fcaa8d6d53b6affcdcb49668d60b0e41d2aa7ff5f4dc2c8fce0d24caa0022"
    lockdc_sha256="0a1890bdd04f377acd95afa699e2cf1ed81ccbacfad95cbd312599915626df99"
    lonejson_sha256="2a73b6bb7e2e690b1accfb811c64de075b2efb569bf9cc299547f4f577c151f7"
    pslog_sha256="f8f4e18810ecad7278eb341fbfe7e3f9d85eb654891c4d08149f425f3a4c9b3d"
    cai_sha256="f18732d80e0510b1ffc32c5218429eed44665927e939ab8e5695984e41934071"
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
unzip_tool=$(resolve_tool_path "${UNZIP:-unzip}")

system_version="0.2.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$downloads_dir/$system_archive"
lockdc_version="0.10.0"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$downloads_dir/$lockdc_archive"
lonejson_version="0.32.0"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$downloads_dir/$lonejson_archive"
lonejson_lua_archive="lonejson-${lonejson_version}-1.src.rock"
lonejson_lua_payload="lonejson-${lonejson_version}.tar.gz"
lonejson_lua_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_lua_archive}"
lonejson_lua_download="$downloads_dir/$lonejson_lua_archive"
lonejson_lua_sha256="87a680b338399de7f981c2dac0681c6cddc8542463d9a3cf6f2f0f17808e47d2"
lonejson_source_dir="$deps_root/share/lonejson-source"
pslog_version="0.4.1"
pslog_archive="libpslog-${pslog_version}-${target_id}.tar.gz"
pslog_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_archive}"
pslog_download="$downloads_dir/$pslog_archive"
cai_version="0.1.2"
cai_archive="cai-${cai_version}-${target_id}.tar.gz"
cai_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_archive}"
cai_download="$downloads_dir/$cai_archive"
cai_lua_archive="cai-${cai_version}-1.src.rock"
cai_lua_payload="cai-lua-${cai_version}.tar.gz"
cai_lua_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_lua_archive}"
cai_lua_download="$downloads_dir/$cai_lua_archive"
cai_lua_sha256="beaae3f21230f472aa70079993919ef5bf1ea84dd57678d573ddace7e417a1eb"
cai_lua_source_dir="$deps_root/share/cai-lua-source"
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
download_if_missing "$lonejson_lua_url" "$lonejson_lua_download"
download_if_missing "$pslog_url" "$pslog_download"
download_if_missing "$cai_url" "$cai_download"
download_if_missing "$cai_lua_url" "$cai_lua_download"
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
actual_lonejson_lua_sha256=$(sha256sum "$lonejson_lua_download" | awk '{print $1}')
if [ "$actual_lonejson_lua_sha256" != "$lonejson_lua_sha256" ]; then
  echo "checksum mismatch for $lonejson_lua_archive" >&2
  echo "expected $lonejson_lua_sha256" >&2
  echo "actual   $actual_lonejson_lua_sha256" >&2
  exit 1
fi
actual_pslog_sha256=$(sha256sum "$pslog_download" | awk '{print $1}')
if [ "$actual_pslog_sha256" != "$pslog_sha256" ]; then
  echo "checksum mismatch for $pslog_archive" >&2
  echo "expected $pslog_sha256" >&2
  echo "actual   $actual_pslog_sha256" >&2
  exit 1
fi
actual_cai_sha256=$(sha256sum "$cai_download" | awk '{print $1}')
if [ "$actual_cai_sha256" != "$cai_sha256" ]; then
  echo "checksum mismatch for $cai_archive" >&2
  echo "expected $cai_sha256" >&2
  echo "actual   $actual_cai_sha256" >&2
  exit 1
fi
actual_cai_lua_sha256=$(sha256sum "$cai_lua_download" | awk '{print $1}')
if [ "$actual_cai_lua_sha256" != "$cai_lua_sha256" ]; then
  echo "checksum mismatch for $cai_lua_archive" >&2
  echo "expected $cai_lua_sha256" >&2
  echo "actual   $actual_cai_lua_sha256" >&2
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
    "-DCMAKE_C_FLAGS=-ffile-prefix-map=$repo_root=. -ffile-prefix-map=$libxml2_source_dir=libxml2 -ffile-prefix-map=$libxml2_build_root=libxml2-build" \
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
    -DLIBXML2_WITH_CATALOG=OFF \
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
mkdir -p "$lonejson_source_dir"
"$unzip_tool" -p "$lonejson_lua_download" "$lonejson_lua_payload" |
  tar -xzf - -C "$lonejson_source_dir" --strip-components 1
case "$target_cmake_system_name" in
  Linux)
    if [ -e "$deps_root/lib/liblonejson.so.0.32.0" ] && [ ! -e "$deps_root/lib/liblonejson.so.16" ]; then
      ln -s liblonejson.so.0.32.0 "$deps_root/lib/liblonejson.so.16"
    fi
    ;;
esac
tar -xzf "$pslog_download" -C "$deps_root" --strip-components 1
tar -xzf "$cai_download" -C "$deps_root" --strip-components 1
mkdir -p "$cai_lua_source_dir"
"$unzip_tool" -p "$cai_lua_download" "$cai_lua_payload" |
  tar -xzf - -C "$cai_lua_source_dir" --strip-components 1
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
lonejson_lua_archive=$lonejson_lua_archive
lonejson_lua_payload=$lonejson_lua_payload
lonejson_lua_sha256=$lonejson_lua_sha256
pslog_archive=$pslog_archive
pslog_version=$pslog_version
pslog_sha256=$pslog_sha256
pslog_source=libpslog-release
cai_archive=$cai_archive
cai_version=$cai_version
cai_sha256=$cai_sha256
cai_lua_archive=$cai_lua_archive
cai_lua_payload=$cai_lua_payload
cai_lua_sha256=$cai_lua_sha256
cai_source=cai-release
cai_lua_source=cai-src-rock
lonejson_lua_source=lonejson-src-rock
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
libxml2_catalog=disabled
libxml2_linkage=static+shared
EOF
