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

case "$preset" in
  deps-host-debug)
    deps_root="$repo_root/.cache/deps/host-debug"
    target_id="x86_64-linux-gnu"
    system_sha256="745fde56d564dcdcb22ed9f16a7b73c8c2e18f947d5fe37ab774e5154ef554b1"
    lockdc_sha256="3c850cc155f7032b60f2c8e078a78cce58e55426b64aeaeaa6bef9a88942e098"
    lonejson_sha256="2626df65f8ac33aadd76b9d33a22fd8038cadf4ee6b7fecad3b60739c359db1a"
    pslog_sha256="91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8"
    cai_sha256="84e401154f7e81707657da3068f0a57b58bf0899c930a296c98ebe46a5619661"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    ;;
  deps-x86_64-linux-gnu)
    deps_root="$repo_root/.cache/deps/x86_64-linux-gnu"
    target_id="x86_64-linux-gnu"
    system_sha256="745fde56d564dcdcb22ed9f16a7b73c8c2e18f947d5fe37ab774e5154ef554b1"
    lockdc_sha256="3c850cc155f7032b60f2c8e078a78cce58e55426b64aeaeaa6bef9a88942e098"
    lonejson_sha256="2626df65f8ac33aadd76b9d33a22fd8038cadf4ee6b7fecad3b60739c359db1a"
    pslog_sha256="91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8"
    cai_sha256="84e401154f7e81707657da3068f0a57b58bf0899c930a296c98ebe46a5619661"
    target_cc="${CC:-cc}"
    target_ar="${AR:-ar}"
    target_ranlib="${RANLIB:-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    target_id="x86_64-linux-musl"
    system_sha256="e3a563a71d6bb9e1e3bdf14343d65bcc0ad8897713e6a8ddfd97cc7bc6b9c6f3"
    lockdc_sha256="6e4c55a229570cd6ad7b888b2298977d8341147862ead915d5cedeaf518352af"
    lonejson_sha256="1d5668be9d88e625735dadaa312bdff7fc0df51754a7891a3efb2be331c2adca"
    pslog_sha256="b628d32f9207e5102c9a8ae3f7ad32ce36e61178c7db67e6aa4548eb9cae567d"
    cai_sha256="ef85c84cdf82f2d37d3db3ebe7c97bcb28dc36072285779df993d8ee8a12d959"
    target_cc="${CC:-x86_64-linux-musl-gcc}"
    target_ar="${AR:-x86_64-linux-musl-ar}"
    target_ranlib="${RANLIB:-x86_64-linux-musl-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    target_id="aarch64-linux-gnu"
    system_sha256="574e02e193330fd8e8fa5c56442ae8d11e9c901b3e89a53f299d66557941b67c"
    lockdc_sha256="f281cf8e01cc80ac38c9d7d9726253f041b642fc5753917ce0fe536eee7f2034"
    lonejson_sha256="949a55b0958f0b4ac16295056a0302613ca2bc02b8dcfa6c6fb356231e7a04bf"
    pslog_sha256="d936ae9416f539c4f40aeaa023b9147cbd568bc87b7a3c3b091adfd217d935bb"
    cai_sha256="4df06b5e31c2f4b98a29031591bd2994aa77d372efc63e702dcffd3ec035968b"
    target_cc="${CC:-aarch64-linux-gnu-gcc}"
    target_ar="${AR:-aarch64-linux-gnu-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-gnu-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    target_id="aarch64-linux-musl"
    system_sha256="c1b1e6b482172760f5967d484d2ef271e1d44defaf7f8f6e35bc66811f4f48fc"
    lockdc_sha256="1972ef73a9963c0a280c93d8b2da1d8e5cbf4da6af0f566f7d3471dfc8370b3a"
    lonejson_sha256="2891e18cfe7843dd7c15e71a4d1bd03dea6f4d178eb189fd8f3b6ad7caf067d7"
    pslog_sha256="638725174cf39f3c5337fc6f118bc88c2a41d385a01be98170ff4bef3d57fcae"
    cai_sha256="04808e14b8c55e2d624ba6ac01866b33b61c4a5913be92418966c557baf393e1"
    target_cc="${CC:-aarch64-linux-musl-gcc}"
    target_ar="${AR:-aarch64-linux-musl-ar}"
    target_ranlib="${RANLIB:-aarch64-linux-musl-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    target_id="armhf-linux-gnu"
    system_sha256="1e4de9dd3de7345629c86cd140177d4dc591c740f7432041bcd423be9bf82496"
    lockdc_sha256="b727af6d49f3de0f229d8d394764fe78d2d66fdd211a1b6b6f2753b8cbd78986"
    lonejson_sha256="0621cae1f1d5a3e8830f34005f3adf9b5fee7195486ab71b649bc8a61ca0c1b6"
    pslog_sha256="bc8530a3773666deb6d551263c7dd59a64c92629fa56d1e89c278d637472f2dc"
    cai_sha256="0ed692d3b14e86e404f8ac5d9982d732291e67f8b56e3e028db4d55192318854"
    target_cc="${CC:-arm-linux-gnueabihf-gcc}"
    target_ar="${AR:-arm-linux-gnueabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-gnueabihf-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    target_id="armhf-linux-musl"
    system_sha256="222236dacf9df80f01fbc14768ed5163e08cbcdb32ae6460a0f494df14330f0b"
    lockdc_sha256="ea6dc0080a8494b206f5d43ec4d3e175f0ee46d2b94d9b06e6b2a2e1ca7a5095"
    lonejson_sha256="975be93eff3e4ed08973f52fc57498d0a5faeec87b1ffa84eed8325c068c5344"
    pslog_sha256="503d2bd882c053dc8f34dbfe718a328303a4973789bbb8fb37261e4822b3babe"
    cai_sha256="c9c899f6881a977a5d88218505649531cdac200a4c969400d5c1a7e88a529056"
    target_cc="${CC:-arm-linux-musleabihf-gcc}"
    target_ar="${AR:-arm-linux-musleabihf-ar}"
    target_ranlib="${RANLIB:-arm-linux-musleabihf-ranlib}"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    target_id="arm64-apple-darwin"
    system_sha256="c5c0160ee65c94084350ee54eb21fc64c410b76c1d104071b29fb5e170ae0081"
    lockdc_sha256="429db62552856cdf81ed60fd9e93d453887790557a8fd565985109d0aeba12ff"
    lonejson_sha256="1e51e13d3850de920f689a245c3372cd6949a8c9531863b574fc05425726be51"
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

system_version="0.4.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$downloads_dir/$system_archive"
lockdc_version="0.12.1"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$downloads_dir/$lockdc_archive"
lockdc_lua_archive="lockdc-${lockdc_version}-1.src.rock"
lockdc_lua_payload="lockdc-${lockdc_version}-1.tar.gz"
lockdc_lua_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_lua_archive}"
lockdc_lua_download="$downloads_dir/$lockdc_lua_archive"
lockdc_lua_sha256="92ac75bec796137782ca08c4f8971bcf4dd4bca86e3376b91366c1c7c177f74b"
lockdc_lua_source_dir="$deps_root/share/lockdc-source"
lonejson_version="0.32.1"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$downloads_dir/$lonejson_archive"
lonejson_lua_archive="lonejson-${lonejson_version}-1.src.rock"
lonejson_lua_payload="lonejson-${lonejson_version}.tar.gz"
lonejson_lua_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_lua_archive}"
lonejson_lua_download="$downloads_dir/$lonejson_lua_archive"
lonejson_lua_sha256="0991c3029539c3716688f53d0ccbefe09cec199816389b865ecb146a665f013d"
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
libxml2_version="2.15.3"
lua_version="5.5.0"
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
download_if_missing "$lockdc_lua_url" "$lockdc_lua_download"
download_if_missing "$lonejson_url" "$lonejson_download"
download_if_missing "$lonejson_lua_url" "$lonejson_lua_download"
download_if_missing "$pslog_url" "$pslog_download"
download_if_missing "$cai_url" "$cai_download"
download_if_missing "$cai_lua_url" "$cai_lua_download"
download_if_missing "$pid0_url" "$pid0_download"

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
actual_lockdc_lua_sha256=$(sha256sum "$lockdc_lua_download" | awk '{print $1}')
if [ "$actual_lockdc_lua_sha256" != "$lockdc_lua_sha256" ]; then
  echo "checksum mismatch for $lockdc_lua_archive" >&2
  echo "expected $lockdc_lua_sha256" >&2
  echo "actual   $actual_lockdc_lua_sha256" >&2
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
rm -rf "$deps_root/include" "$deps_root/lib" "$deps_root/share"
mkdir -p "$deps_root"
tar -xzf "$system_download" -C "$deps_root" --strip-components 1
tar -xzf "$lockdc_download" -C "$deps_root" --strip-components 1
mkdir -p "$lockdc_lua_source_dir"
"$unzip_tool" -p "$lockdc_lua_download" "$lockdc_lua_payload" |
  tar -xzf - -C "$lockdc_lua_source_dir" --strip-components 1
tar -xzf "$lonejson_download" -C "$deps_root" --strip-components 1
mkdir -p "$lonejson_source_dir"
"$unzip_tool" -p "$lonejson_lua_download" "$lonejson_lua_payload" |
  tar -xzf - -C "$lonejson_source_dir" --strip-components 1
case "$target_cmake_system_name" in
  Linux)
    if [ -e "$deps_root/lib/liblonejson.so.$lonejson_version" ] && [ ! -e "$deps_root/lib/liblonejson.so.16" ]; then
      ln -s "liblonejson.so.$lonejson_version" "$deps_root/lib/liblonejson.so.16"
    fi
    ;;
esac
tar -xzf "$pslog_download" -C "$deps_root" --strip-components 1
tar -xzf "$cai_download" -C "$deps_root" --strip-components 1
mkdir -p "$cai_lua_source_dir"
"$unzip_tool" -p "$cai_lua_download" "$cai_lua_payload" |
  tar -xzf - -C "$cai_lua_source_dir" --strip-components 1
gzip -dc "$pid0_download" > "$deps_root/include/$pid0_header"

cat > "$manifest_path" <<EOF
preset=$preset
system_archive=$system_archive
system_version=$system_version
system_sha256=$system_sha256
liblockdc_archive=$lockdc_archive
liblockdc_version=$lockdc_version
liblockdc_sha256=$lockdc_sha256
lockdc_lua_archive=$lockdc_lua_archive
lockdc_lua_payload=$lockdc_lua_payload
lockdc_lua_sha256=$lockdc_lua_sha256
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
lockdc_lua_source=lockdc-src-rock
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
lua_source=c.pkt.systems
lua_linkage=static+shared
lua_runtime_facade=cpkt-lua-runtime
lua_runtime_facade_abi=0
libxml2_version=$libxml2_version
libxml2_source=c.pkt.systems
libxml2_iconv=enabled
libxml2_zlib=enabled
libxml2_catalog=default
libxml2_linkage=static+shared
EOF
