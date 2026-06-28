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
  class=${3:-relocatability}
  echo "$reason${artifact:+: $artifact}" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=verify-release-privacy
phase=privacy-relocatability
status=failed
class=$class
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

resolve_executable() {
  tool=$1
  [ -n "$tool" ] || return 1
  case "$tool" in
    */*) [ -x "$tool" ] && printf '%s\n' "$tool" ;;
    *) command -v "$tool" 2>/dev/null ;;
  esac
}

discover_darwin_otool() {
  target_id=$1

  if resolved=$(resolve_executable "${VECTIS_OTOOL:-}" 2>/dev/null); then
    printf '%s\n' "$resolved"
    return 0
  fi

  build_dir="${VECTIS_BINARY_DIR:-$repo_root/build/$target_id-release}"
  if [ -f "$build_dir/CMakeCache.txt" ]; then
    eval "$("$script_dir/discover_target_tools.sh" --build-dir "$build_dir" --target-id "$target_id")"
    if resolved=$(resolve_executable "${OTOOL:-}" 2>/dev/null); then
      printf '%s\n' "$resolved"
      return 0
    fi
  fi

  fail "target-correct Darwin otool unavailable" "$target_id" "external-tool-unavailable"
}

allowed_darwin_absolute_path() {
  case "$1" in
    /usr/lib/*|/System/Library/*) return 0 ;;
  esac
  return 1
}

allowed_darwin_load_path() {
  case "$1" in
    @rpath/*|@loader_path|@loader_path/*|@executable_path|@executable_path/*) return 0 ;;
  esac
  allowed_darwin_absolute_path "$1"
}

check_darwin_load_path() {
  file=$1
  path=$2
  reason=$3

  [ -n "$path" ] || return 0
  if ! allowed_darwin_load_path "$path"; then
    fail "$reason" "$file: $path"
  fi
}

verify_darwin_install_name() {
  otool=$1
  file=$2
  name=${file##*/}
  install_name=

  case "$name" in
    *.dylib) ;;
    *) return 0 ;;
  esac

  install_name=$("$otool" -D "$file" 2>/dev/null | sed -n '2p' | awk '{print $1}')
  [ -n "$install_name" ] || fail "missing Darwin dylib install name" "$file"

  case "$name" in
    libvectis*.dylib)
      case "$install_name" in
        @rpath/*) ;;
        *) fail "Darwin project dylib install name is not @rpath-relative" "$file: $install_name" ;;
      esac
      ;;
  esac

  check_darwin_load_path "$file" "$install_name" "non-relocatable Darwin dylib install name"
}

verify_darwin_dependencies() {
  otool=$1
  file=$2
  loads=$3

  printf '%s\n' "$loads" | sed '1d' |
  while IFS= read -r line; do
    path=$(printf '%s\n' "$line" | awk '{print $1}')
    [ -n "$path" ] || continue
    check_darwin_load_path "$file" "$path" "non-system absolute Darwin dependency path"
  done
}

verify_darwin_rpaths_and_signature() {
  file=$1
  load_commands=$2

  printf '%s\n' "$load_commands" | awk '
    $1 == "cmd" && $2 == "LC_CODE_SIGNATURE" { print "codesig"; next }
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
    in_rpath && $1 == "path" { print "rpath " $2; in_rpath = 0; next }
    $1 == "cmd" { in_rpath = 0 }
  ' |
  while IFS= read -r record; do
    case "$record" in
      "rpath "*)
        path=${record#rpath }
        case "$path" in
          @loader_path|@loader_path/*|@executable_path|@executable_path/*) ;;
          *) fail "non-relocatable Darwin rpath" "$file: $path" ;;
        esac
        ;;
      codesig)
        ;;
    esac
  done
}

verify_darwin_root() {
  package_root=$1
  target_id=$2
  otool=$(discover_darwin_otool "$target_id")

  find "$package_root" \( -path "$package_root/bin/*" -o -path "$package_root/lib/*" \) -type f -print |
  while IFS= read -r file; do
    case "$file" in
      *.a) continue ;;
    esac
    if loads=$("$otool" -L "$file" 2>/dev/null); then
      verify_darwin_install_name "$otool" "$file"
      verify_darwin_dependencies "$otool" "$file" "$loads"
      load_commands=$("$otool" -l "$file" 2>/dev/null || true)
      verify_darwin_rpaths_and_signature "$file" "$load_commands"
    fi
  done
}

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

find "$work_root/extract" -type d -name "vectis-$version-arm64-apple-darwin" -print |
while IFS= read -r package_root; do
  verify_darwin_root "$package_root" arm64-apple-darwin
done

echo "release privacy ok"
