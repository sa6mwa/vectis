#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: scripts/verify_darwin_smoke_bundle.sh --zip path [--require-spctl]

Verifies an arm64 Darwin smoke-test zip produced by
make release-darwin-smoke-bundle. The zip must contain one root directory with
run-smoke.sh and bin/vectis_static_smoke. The verifier checks Mach-O arm64
metadata, verifies codesign without mutation, optionally requires spctl, and
runs the smoke command.

Tool overrides:
  VECTIS_FILE      file tool path
  VECTIS_CODESIGN  codesign tool path
  VECTIS_SPCTL     spctl tool path
  VECTIS_UNZIP     unzip tool path
USAGE
  exit 2
}

resolve_tool() {
  command -v "$1" 2>/dev/null || return 1
}

file_sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    printf '%s\n' 'verify_darwin_smoke_bundle: no SHA-256 tool available' >&2
    return 1
  fi
}

assert_same_hash() {
  if [ "$1" != "$2" ]; then
    printf 'verify_darwin_smoke_bundle: binary changed during %s: %s\n' \
      "$3" "$4" >&2
    exit 1
  fi
}

zip_path=
require_spctl=0
file_tool=${VECTIS_FILE:-file}
codesign_tool=${VECTIS_CODESIGN:-codesign}
spctl_tool=${VECTIS_SPCTL:-spctl}
unzip_tool=${VECTIS_UNZIP:-unzip}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --zip)
      [ "$#" -ge 2 ] || usage
      zip_path=$2
      shift 2
      ;;
    --require-spctl)
      require_spctl=1
      shift
      ;;
    -h|--help)
      usage
      ;;
    *)
      printf 'verify_darwin_smoke_bundle: unknown argument: %s\n' "$1" >&2
      usage
      ;;
  esac
done

[ -n "$zip_path" ] || usage
[ -f "$zip_path" ] || {
  printf 'verify_darwin_smoke_bundle: zip not found: %s\n' "$zip_path" >&2
  exit 1
}

file_bin=$(resolve_tool "$file_tool") || {
  printf 'verify_darwin_smoke_bundle: file tool unavailable: %s\n' \
    "$file_tool" >&2
  exit 1
}
codesign_bin=$(resolve_tool "$codesign_tool") || {
  printf 'verify_darwin_smoke_bundle: codesign unavailable: %s\n' \
    "$codesign_tool" >&2
  exit 1
}
unzip_bin=$(resolve_tool "$unzip_tool") || {
  printf 'verify_darwin_smoke_bundle: unzip unavailable: %s\n' \
    "$unzip_tool" >&2
  exit 1
}

work_root=$(mktemp -d "${TMPDIR:-/tmp}/vectis-darwin-smoke-verify.XXXXXX")
trap 'rm -rf "$work_root"' EXIT INT TERM

"$unzip_bin" -q "$zip_path" -d "$work_root"
root_count=$(find "$work_root" -mindepth 1 -maxdepth 1 -type d |
  wc -l | tr -d ' ')
if [ "$root_count" != "1" ]; then
  printf '%s\n' \
    'verify_darwin_smoke_bundle: zip must contain exactly one root directory' >&2
  find "$work_root" -mindepth 1 -maxdepth 1 -print >&2
  exit 1
fi
root=$(find "$work_root" -mindepth 1 -maxdepth 1 -type d -print)

if [ ! -x "$root/run-smoke.sh" ]; then
  printf '%s\n' \
    'verify_darwin_smoke_bundle: bundle missing executable run-smoke.sh' >&2
  exit 1
fi
if [ ! -x "$root/bin/vectis_static_smoke" ]; then
  printf '%s\n' \
    'verify_darwin_smoke_bundle: bundle missing executable bin/vectis_static_smoke' >&2
  exit 1
fi

"$file_bin" "$root/bin/vectis_static_smoke" |
  grep -E 'Mach-O .*arm64' >/dev/null || {
    printf '%s\n' \
      'verify_darwin_smoke_bundle: smoke binary is not an arm64 Mach-O executable' >&2
    exit 1
  }

find "$root/bin" "$root/lib" \
  -type f \( -perm -111 -o -name '*.dylib' \) -print 2>/dev/null |
  sort >"$work_root/macho-files.txt"
if [ ! -s "$work_root/macho-files.txt" ]; then
  printf '%s\n' \
    'verify_darwin_smoke_bundle: bundle does not contain executable Mach-O files' >&2
  exit 1
fi

while IFS= read -r binary; do
  before=$(file_sha256 "$binary")
  "$codesign_bin" --verify --strict --verbose=4 "$binary"
  after=$(file_sha256 "$binary")
  assert_same_hash "$before" "$after" "codesign verification" "$binary"

  if [ -x "$binary" ]; then
    if spctl_bin=$(resolve_tool "$spctl_tool"); then
      before_spctl=$(file_sha256 "$binary")
      if [ "$require_spctl" -ne 0 ]; then
        "$spctl_bin" --assess --type execute "$binary"
      elif ! "$spctl_bin" --assess --type execute "$binary"; then
        printf 'verify_darwin_smoke_bundle: skipping failed optional spctl assessment: %s\n' \
          "$binary" >&2
      fi
      after_spctl=$(file_sha256 "$binary")
      assert_same_hash "$before_spctl" "$after_spctl" "spctl assessment" \
        "$binary"
    elif [ "$require_spctl" -ne 0 ]; then
      printf 'verify_darwin_smoke_bundle: spctl unavailable: %s\n' \
        "$spctl_tool" >&2
      exit 1
    fi
  fi
done <"$work_root/macho-files.txt"

"$root/run-smoke.sh"

printf 'darwin smoke bundle verification ok: %s\n' "$zip_path"
