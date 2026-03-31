#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
preset=${1:-deps-host-debug}

downloads_dir="$repo_root/.cache/downloads"
deps_root=
lockdc_arch=
lockdc_libc=

case "$preset" in
  deps-host-debug)
    deps_root="$repo_root/.cache/deps/host-debug"
    lockdc_arch="x86_64"
    lockdc_libc="gnu"
    ;;
  deps-x86_64-linux-gnu)
    deps_root="$repo_root/.cache/deps/x86_64-linux-gnu"
    lockdc_arch="x86_64"
    lockdc_libc="gnu"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    lockdc_arch="x86_64"
    lockdc_libc="musl"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    lockdc_arch="aarch64"
    lockdc_libc="gnu"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    lockdc_arch="aarch64"
    lockdc_libc="musl"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    lockdc_arch="armhf"
    lockdc_libc="gnu"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    lockdc_arch="armhf"
    lockdc_libc="musl"
    ;;
  *)
    echo "usage: scripts/deps.sh [deps-host-debug|deps-x86_64-linux-gnu|deps-x86_64-linux-musl|deps-aarch64-linux-gnu|deps-aarch64-linux-musl|deps-armhf-linux-gnu|deps-armhf-linux-musl]" >&2
    exit 2
    ;;
esac

mkdir -p "$downloads_dir" "$deps_root/include" "$deps_root/lib"

lockdc_archive="liblockdc-0.1.0-${lockdc_arch}-linux-${lockdc_libc}-dev.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v0.1.0/${lockdc_archive}"
lockdc_download="$downloads_dir/$lockdc_archive"
pslog_download="$downloads_dir/pslog-0.2.0.h.gz"
lonejson_download="$downloads_dir/lonejson-0.1.0.h.gz"
manifest_path="$deps_root/manifest.txt"

download_if_missing() {
  url=$1
  out=$2
  if [ ! -f "$out" ]; then
    curl -L --fail --retry 3 --output "$out" "$url"
  fi
}

download_if_missing "$lockdc_url" "$lockdc_download"
download_if_missing "https://github.com/sa6mwa/libpslog/releases/download/v0.2.0/pslog-0.2.0.h.gz" "$pslog_download"
download_if_missing "https://github.com/sa6mwa/lonejson/releases/download/v0.1.0/lonejson-0.1.0.h.gz" "$lonejson_download"

rm -rf "$deps_root/include" "$deps_root/lib" "$deps_root/share"
mkdir -p "$deps_root"
tar -xzf "$lockdc_download" -C "$deps_root"
gzip -dc "$pslog_download" > "$deps_root/include/pslog.h"
gzip -dc "$lonejson_download" > "$deps_root/include/lonejson.h"

cat > "$manifest_path" <<EOF
preset=$preset
liblockdc_archive=$lockdc_archive
pslog_version=0.2.0
lonejson_version=0.1.0
EOF

