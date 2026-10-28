#!/usr/bin/env bash
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source "$script_dir/require-host-bash.sh"
source "$script_dir/cpkt-archive-cache.sh"
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
"${CMAKE:-cmake}" -P "$repo_root/cmake/vectis_owned_paths.cmake"
preset=${1:-deps-host-debug}
case "$preset" in
  deps-host-debug)
    case "${VECTIS_HOST_UNAME_S:-$(uname -s)}:${VECTIS_HOST_UNAME_M:-$(uname -m)}" in
      Linux:x86_64|Linux:amd64) target=x86_64-linux-gnu;;
      Linux:aarch64|Linux:arm64) target=aarch64-linux-gnu;;
      Linux:armv7l|Linux:armv6l|Linux:armv8l) target=armhf-linux-gnu;;
      Darwin:arm64) target=arm64-apple-darwin;;
      *) echo 'unsupported native dependency target' >&2; exit 2;;
    esac
    ;;
  deps-x86_64-linux-gnu|deps-x86_64-linux-musl|deps-aarch64-linux-gnu|deps-aarch64-linux-musl|deps-armhf-linux-gnu|deps-armhf-linux-musl|deps-arm64-apple-darwin)
    target=${preset#deps-};;
  *) echo "unknown dependency selection: $preset" >&2; exit 2;;
esac
deps_root="$repo_root/.cache/deps/${preset#deps-}"
global_cache="${XDG_CACHE_HOME:-${HOME:?HOME or XDG_CACHE_HOME is required}/.cache}/cpkt/deps"
dependency_cache=${CPKT_DEPENDENCY_CACHE:-$global_cache}
[[ "$dependency_cache" == /* ]] || { echo 'dependency cache must be absolute' >&2; exit 2; }
dependency_cache=$(realpath -m -- "$dependency_cache")
global_cache=$(realpath -m -- "$global_cache")
case "$dependency_cache" in
  "$global_cache"|"$repo_root/build/"*|"$repo_root/.cache/"*) ;;
  *) echo 'CPKT_DEPENDENCY_CACHE must use this checkout build/.cache or the declared global cpkt cache' >&2; exit 2;;
esac
case "/$dependency_cache/" in */../*|*/./*) echo 'dependency cache must be an absolute normalized path' >&2; exit 2;; esac
[[ "$(realpath -m -- "$deps_root")" == "$deps_root" ]] || {
  echo 'dependency extraction cannot borrow another checkout through a symlink' >&2; exit 2;
}
mkdir -p "$deps_root/.prepared" "$dependency_cache/archives/sha256" "$dependency_cache/locks"

# Preparation is explicit here; configure and try_compile only discover tools.
if [[ "$target" != arm64-apple-darwin || "$(uname -s)" != Darwin ]]; then
  "$script_dir/cpkt-toolchains.sh" ensure "$target"
fi

acquire() (
  local url=$1 digest=$2 name=${1##*/} archive_dir temporary actual
  archive_dir="$dependency_cache/archives/sha256/$digest"
  mkdir -p "$archive_dir"
  exec 9>"$dependency_cache/locks/$digest.lock"
  flock -w "${CPKT_DEPENDENCY_LOCK_TIMEOUT:-600}" 9
  cpkt_restore_cached_archive "$archive_dir/$name" "$digest"
  if [[ ! -f "$archive_dir/$name" ]]; then
    temporary=$(mktemp "$archive_dir/.$name.XXXXXXXX")
    trap 'rm -f -- "$temporary"' EXIT
    curl --fail --location --retry 3 --connect-timeout 20 --output "$temporary" "$url"
    actual=$(cpkt_cached_archive_sha256 "$temporary")
    [[ "$actual" == "$digest" ]] || { echo "checksum mismatch: $name" >&2; exit 1; }
    mv -- "$temporary" "$archive_dir/$name"
  fi
)

manifest_tmp=$(mktemp "$deps_root/.manifest.XXXXXXXX")
staging=
trap 'rm -f -- "$manifest_tmp"; if [[ -n "$staging" ]]; then rm -rf -- "$staging"; fi' EXIT
printf 'preset=%s\ntarget_id=%s\n' "$preset" "$target" >"$manifest_tmp"
while IFS=$'\t' read -r kind name version pin_target digest url; do
  [[ "$kind" != \#* ]] || continue
  [[ "$pin_target" == "$target" || "$pin_target" == all ||
     ( "$pin_target" == linux && "$target" == *-linux-* ) ]] || continue
  [[ "$digest" =~ ^[0-9a-f]{64}$ && "$name" =~ ^[a-z0-9-]+$ && "$url" == https://* ]] || {
    echo 'invalid dependency pin' >&2; exit 1;
  }
  case "$kind" in
    sdk|header) prefix="$deps_root/$name";;
    lua) prefix="$deps_root/sources/$name";;
    *) echo "invalid dependency kind: $kind" >&2; exit 1;;
  esac
  [[ "$(realpath -m -- "$prefix")" == "$prefix" ]] || {
    echo "dependency prefix cannot borrow another checkout through a symlink: $prefix" >&2; exit 2;
  }
  marker="$deps_root/.prepared/$kind-$name"
  acquire "$url" "$digest"
  archive="$dependency_cache/archives/sha256/$digest/${url##*/}"
  if [[ ! -d "$prefix" || ! -f "$marker" || "$(cat "$marker")" != "$digest" ]]; then
    staging=$(mktemp -d "$deps_root/.extract-$name.XXXXXXXX")
    if [[ "$kind" == header ]]; then
      mkdir -p "$staging/include"
      gzip -dc "$archive" >"$staging/include/libpid0.h"
    else
      tar -xpzf "$archive" -C "$staging" --strip-components=1
    fi
    mkdir -p "$(dirname -- "$prefix")"
    rm -rf -- "$prefix"
    mv -- "$staging" "$prefix"
    staging=
    printf '%s\n' "$digest" >"$marker"
  fi
  if [[ "$kind" == lua ]]; then key="${name//-/_}_lua"; else key="${name//-/_}"; fi
  printf '%s_version=%s\n%s_archive=%s\n%s_sha256=%s\n' \
    "$key" "$version" "$key" "${url##*/}" "$key" "$digest" >>"$manifest_tmp"
done <"$script_dir/dependency-pins.tsv"
# Retire the owned monolithic layout only after all replacement prefixes are
# prepared. Otherwise old headers can silently precede imported-target headers.
if [[ -f "$deps_root/share/c.pkt.systems/manifest.txt" ]]; then
  rm -rf -- "$deps_root/include" "$deps_root/lib" "$deps_root/share" "$deps_root/bin"
fi
if [[ ! -f "$deps_root/manifest.txt" ]] || ! cmp -s "$manifest_tmp" "$deps_root/manifest.txt"; then
  mv -- "$manifest_tmp" "$deps_root/manifest.txt"
fi
printf 'Prepared independent dependency prefixes for %s in %s\n' "$target" "$deps_root"
