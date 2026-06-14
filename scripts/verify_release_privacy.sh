#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${VECTIS_VERSION:-$("$script_dir/release_version.sh")}
dist_dir=${VECTIS_DIST_DIR:-"$repo_root/dist"}
checksums="$dist_dir/vectis-$version-CHECKSUMS"
work_root="$repo_root/build/release-privacy-verify"

fail() {
  reason=$1
  artifact=${2:-}
  echo "$reason${artifact:+: $artifact}" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=verify-release-privacy
phase=privacy-relocatability
status=failed
class=relocatability
reason=$reason
artifact=$artifact
next=remove local paths or non-relocatable runtime metadata before release
PKT_DIAGNOSTIC_END
EOF
  exit 1
}

[ -f "$checksums" ] || fail "missing checksum manifest" "$checksums"

rm -rf "$work_root"
mkdir -p "$work_root/extract"

scan_path() {
  needle=$1
  label=$2
  if [ -n "$needle" ]; then
    find "$work_root/extract" -type f -print |
    while IFS= read -r file; do
      if strings -a "$file" | grep -F -n -m 1 "$needle" >/tmp/vectis-privacy-hit.$$ 2>/dev/null; then
        hit=$(sed -n '1p' /tmp/vectis-privacy-hit.$$)
        rm -f /tmp/vectis-privacy-hit.$$
        fail "$label leaked into release artifact" "$file:$hit"
      fi
      rm -f /tmp/vectis-privacy-hit.$$
    done
  fi
}

while read -r _hash artifact_name; do
  artifact="$dist_dir/$artifact_name"
  case "$artifact_name" in
    *.tar.gz)
      mkdir -p "$work_root/extract/$artifact_name"
      tar -C "$work_root/extract/$artifact_name" -xzf "$artifact"
      ;;
    *.zip)
      mkdir -p "$work_root/extract/$artifact_name"
      if command -v unzip >/dev/null 2>&1; then
        unzip -q "$artifact" -d "$work_root/extract/$artifact_name"
      else
        (cd "$work_root/extract/$artifact_name" && "${CMAKE:-cmake}" -E tar xf "$artifact")
      fi
      ;;
  esac
done <"$checksums"

find "$work_root/extract" -type f \( -name '*.tar.gz' -o -name '*.tgz' -o -name '*.zip' -o -name '*.rock' -o -name '*.src.rock' \) |
while IFS= read -r nested; do
  nested_dir="$nested.expanded"
  mkdir -p "$nested_dir"
  case "$nested" in
    *.tar.gz|*.tgz) tar -C "$nested_dir" -xzf "$nested" || true ;;
    *.zip|*.rock|*.src.rock)
      if command -v unzip >/dev/null 2>&1; then
        unzip -q "$nested" -d "$nested_dir" || true
      fi
      ;;
  esac
done

scan_path "file://$repo_root" "repository file URL"
scan_path "file://${HOME:-}" "home file URL"
scan_path "$repo_root" "repository path"
scan_path "${HOME:-}" "home path"
scan_path "$repo_root/.cache" "dependency cache path"
scan_path "$repo_root/build" "build path"

verify_elf_runtime_paths() {
  file=$1
  dynamic=$2

  while IFS= read -r line; do
    paths=$(printf '%s\n' "$line" | sed -n 's/.*\[\(.*\)\].*/\1/p')
    [ -n "$paths" ] || continue
    old_ifs=$IFS
    IFS=:
    for entry in $paths; do
      IFS=$old_ifs
      case "$entry" in
        '$ORIGIN'|'$ORIGIN/'*) ;;
        *)
          fail "non-relocatable ELF runtime path" "$file: $line"
          ;;
      esac
      IFS=:
    done
    IFS=$old_ifs
  done < <(printf '%s\n' "$dynamic" | grep -E 'RPATH|RUNPATH' || true)
}

if command -v readelf >/dev/null 2>&1; then
  find "$work_root/extract" -type f -print |
  while IFS= read -r file; do
    if readelf -h "$file" >/dev/null 2>&1; then
      dynamic=$(readelf -d "$file" 2>/dev/null || true)
      verify_elf_runtime_paths "$file" "$dynamic"
    fi
  done
fi

echo "release privacy ok"
