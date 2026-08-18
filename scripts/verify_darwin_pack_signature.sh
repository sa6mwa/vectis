#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: scripts/verify_darwin_pack_signature.sh --binary path
       [--codesign-tool path] [--spctl-tool path] [--require-spctl]

Verifies a final Darwin packed executable after all payload embedding,
install-name/rpath updates, stripping, and signing have completed.
USAGE
}

resolve_tool() {
  local tool=$1
  case "$tool" in
    */*)
      [ -x "$tool" ] && printf '%s\n' "$tool" && return 0
      return 1
      ;;
    *)
      command -v "$tool" 2>/dev/null || return 1
      ;;
  esac
}

file_sha256() {
  local file=$1
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$file" | awk '{print $1}'
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$file" | awk '{print $1}'
    return 0
  fi
  printf '%s\n' 'verify_darwin_pack_signature: no SHA-256 tool available' >&2
  return 69
}

assert_same_hash() {
  local before=$1
  local after=$2
  local phase=$3
  if [ "$before" != "$after" ]; then
    printf 'verify_darwin_pack_signature: binary changed during %s: %s\n' \
      "$phase" "$binary" >&2
    exit 74
  fi
}

binary=
codesign_tool=${VECTIS_CODESIGN:-codesign}
spctl_tool=${VECTIS_SPCTL:-spctl}
require_spctl=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --binary)
      [ "$#" -ge 2 ] || { usage; exit 64; }
      binary=$2
      shift 2
      ;;
    --codesign-tool)
      [ "$#" -ge 2 ] || { usage; exit 64; }
      codesign_tool=$2
      shift 2
      ;;
    --spctl-tool)
      [ "$#" -ge 2 ] || { usage; exit 64; }
      spctl_tool=$2
      shift 2
      ;;
    --require-spctl)
      require_spctl=1
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      printf 'verify_darwin_pack_signature: unknown argument: %s\n' "$1" >&2
      usage
      exit 64
      ;;
  esac
done

if [ -z "$binary" ]; then
  printf '%s\n' 'verify_darwin_pack_signature: --binary is required' >&2
  usage
  exit 64
fi
if [ ! -f "$binary" ]; then
  printf 'verify_darwin_pack_signature: binary not found: %s\n' "$binary" >&2
  exit 66
fi
if [ ! -x "$binary" ]; then
  printf 'verify_darwin_pack_signature: binary is not executable: %s\n' \
    "$binary" >&2
  exit 65
fi

if ! codesign_bin=$(resolve_tool "$codesign_tool"); then
  printf 'verify_darwin_pack_signature: codesign unavailable: %s\n' \
    "$codesign_tool" >&2
  exit 69
fi

before_hash=$(file_sha256 "$binary")
"$codesign_bin" --verify --strict --verbose=4 "$binary"
after_codesign_hash=$(file_sha256 "$binary")
assert_same_hash "$before_hash" "$after_codesign_hash" "codesign verification"

if spctl_bin=$(resolve_tool "$spctl_tool"); then
  "$spctl_bin" --assess --type execute "$binary"
  after_spctl_hash=$(file_sha256 "$binary")
  assert_same_hash "$before_hash" "$after_spctl_hash" "spctl assessment"
elif [ "$require_spctl" -ne 0 ]; then
  printf 'verify_darwin_pack_signature: spctl unavailable: %s\n' \
    "$spctl_tool" >&2
  exit 69
else
  printf 'verify_darwin_pack_signature: skipping spctl, tool unavailable: %s\n' \
    "$spctl_tool" >&2
fi

printf 'darwin pack signature verification ok: %s\n' "$binary"
