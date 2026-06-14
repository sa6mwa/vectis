#!/usr/bin/env bash
set -eu

build_dir=
target_id=

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir)
      build_dir=${2:?--build-dir requires a value}
      shift 2
      ;;
    --target-id)
      target_id=${2:?--target-id requires a value}
      shift 2
      ;;
    *)
      echo "usage: scripts/discover_target_tools.sh --build-dir DIR --target-id TARGET" >&2
      exit 2
      ;;
  esac
done

[ -n "$build_dir" ] || { echo "--build-dir is required" >&2; exit 2; }
[ -n "$target_id" ] || { echo "--target-id is required" >&2; exit 2; }

cache="$build_dir/CMakeCache.txt"
[ -f "$cache" ] || { echo "missing CMake cache: $cache" >&2; exit 1; }

cache_value() {
  key=$1
  sed -n "s/^$key:[^=]*=//p;s/^$key=//p" "$cache" | sed -n '1p'
}

resolve_path() {
  tool=$1
  [ -n "$tool" ] || return 1
  case "$tool" in
    */*)
      [ -x "$tool" ] || return 1
      printf '%s\n' "$tool"
      ;;
    *)
      command -v "$tool" 2>/dev/null || return 1
      ;;
  esac
}

compiler=$(cache_value CMAKE_C_COMPILER)
compiler_dir=
compiler_name=
if [ -n "$compiler" ]; then
  case "$compiler" in
    */*)
      compiler_dir=${compiler%/*}
      compiler_name=${compiler##*/}
      ;;
  esac
fi

host_prefix=
case "$target_id" in
  arm64-apple-darwin)
    host_prefix=${CPKT_OSXCROSS_HOST:-}
    if [ -z "$host_prefix" ] && [ -n "$compiler_name" ]; then
      host_prefix=$(printf '%s\n' "$compiler_name" | sed -n 's/^\(.*apple-darwin[0-9]*\)-\(cc\|clang\)$/\1/p')
    fi
    if [ -z "$host_prefix" ]; then
      host_prefix=arm64-apple-darwin25
    fi
    ;;
esac

find_tool() {
  key=$1
  cache_key=$2
  default_name=$3
  prefixed_name=$4
  override_var="VECTIS_$key"
  override=${!override_var:-}

  if resolved=$(resolve_path "$override" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi
  if resolved=$(resolve_path "$(cache_value "$cache_key")" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi
  if [ -n "$compiler_dir" ] && [ -n "$prefixed_name" ] &&
     resolved=$(resolve_path "$compiler_dir/$prefixed_name" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi
  if [ -n "$compiler_dir" ] &&
     resolved=$(resolve_path "$compiler_dir/$default_name" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi
  if [ -n "$prefixed_name" ] &&
     resolved=$(resolve_path "$prefixed_name" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi
  if [ "$target_id" = "arm64-apple-darwin" ] &&
     [ "$key" != "READELF" ]; then
    printf '%s\n' ""
    return 0
  fi
  if resolved=$(resolve_path "$default_name" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi
  printf '%s\n' ""
}

prefixed_strip=
prefixed_install_name_tool=
prefixed_otool=
prefixed_readelf=
if [ -n "$host_prefix" ]; then
  prefixed_strip="$host_prefix-strip"
  prefixed_install_name_tool="$host_prefix-install_name_tool"
  prefixed_otool="$host_prefix-otool"
elif [ -n "$target_id" ]; then
  prefixed_strip="$target_id-strip"
  prefixed_readelf="$target_id-readelf"
fi

cc=$(resolve_path "$compiler" 2>/dev/null || printf '%s\n' "$compiler")
strip_tool=$(find_tool STRIP CMAKE_STRIP strip "$prefixed_strip")
install_name_tool=$(find_tool INSTALL_NAME_TOOL CMAKE_INSTALL_NAME_TOOL install_name_tool "$prefixed_install_name_tool")
otool=$(find_tool OTOOL CMAKE_OTOOL otool "$prefixed_otool")
if [ -z "$otool" ]; then
  otool=$(find_tool OTOOL CPKT_OTOOL otool "$prefixed_otool")
fi
readelf_tool=$(find_tool READELF CMAKE_READELF readelf "$prefixed_readelf")

quote() {
  printf "%s='%s'\n" "$1" "$(printf '%s' "$2" | sed "s/'/'\\\\''/g")"
}

quote CC "$cc"
quote STRIP "$strip_tool"
quote INSTALL_NAME_TOOL "$install_name_tool"
quote OTOOL "$otool"
quote READELF "$readelf_tool"
quote TARGET_HOST_PREFIX "$host_prefix"
