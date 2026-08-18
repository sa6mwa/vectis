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

"$codesign_bin" --verify --strict --verbose=4 "$binary"

if spctl_bin=$(resolve_tool "$spctl_tool"); then
  "$spctl_bin" --assess --type execute "$binary"
elif [ "$require_spctl" -ne 0 ]; then
  printf 'verify_darwin_pack_signature: spctl unavailable: %s\n' \
    "$spctl_tool" >&2
  exit 69
else
  printf 'verify_darwin_pack_signature: skipping spctl, tool unavailable: %s\n' \
    "$spctl_tool" >&2
fi

printf 'darwin pack signature verification ok: %s\n' "$binary"
