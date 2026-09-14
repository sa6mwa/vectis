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
  next="remove local paths or non-relocatable runtime metadata before release"
  if [ "$class" = "static-linkage" ]; then
    next="link bin/vectis without non-system dynamic dependencies before release"
  fi
  echo "$reason${artifact:+: $artifact}" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=verify-release-privacy
phase=privacy-relocatability
status=failed
class=$class
reason=$reason
artifact=$artifact
next=$next
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
      if strings -a "$file" | grep -F -n -m 1 "$needle" >"$work_root/privacy-hit" 2>/dev/null; then
        hit=$(sed -n '1p' "$work_root/privacy-hit")
        fail "$label leaked into release artifact" "$file:$hit"
      fi
    done
  fi
}

while read -r _hash artifact_name; do
  artifact="$dist_dir/$artifact_name"
  case "$artifact_name" in
    *.rockspec)
      mkdir -p "$work_root/extract/$artifact_name"
      cp "$artifact" "$work_root/extract/$artifact_name/"
      ;;
    *.rock|*.src.rock)
      mkdir -p "$work_root/extract/$artifact_name"
      if command -v unzip >/dev/null 2>&1; then
        unzip -q "$artifact" -d "$work_root/extract/$artifact_name"
      else
        (cd "$work_root/extract/$artifact_name" && "${CMAKE:-cmake}" -E tar xf "$artifact")
      fi
      ;;
    *.tar.gz|*.tgz|*.tar.xz)
      mkdir -p "$work_root/extract/$artifact_name"
      tar -C "$work_root/extract/$artifact_name" -xf "$artifact"
      ;;
    *.zip)
      mkdir -p "$work_root/extract/$artifact_name"
      if command -v unzip >/dev/null 2>&1; then
        unzip -q "$artifact" -d "$work_root/extract/$artifact_name"
      else
        (cd "$work_root/extract/$artifact_name" && "${CMAKE:-cmake}" -E tar xf "$artifact")
      fi
      ;;
    *.gz)
      mkdir -p "$work_root/extract/$artifact_name"
      gzip -dc "$artifact" >"$work_root/extract/$artifact_name/payload" || fail "cannot expand compressed artifact" "$artifact"
      ;;
    *)
      mkdir -p "$work_root/extract/$artifact_name"
      cp "$artifact" "$work_root/extract/$artifact_name/payload"
      ;;
  esac
done <"$checksums"

expand_nested() {
  local root=$1 depth=$2 nested nested_dir
  local -a archives
  mapfile -d '' -t archives < <(find "$root" -type f \( -name '*.tar.gz' -o -name '*.tgz' -o -name '*.tar.xz' -o -name '*.zip' -o -name '*.rock' \) -print0)
  for nested in "${archives[@]}"; do
    [ "$depth" -lt 16 ] || fail "archive nesting limit exceeded" "$nested"
    nested_dir="$nested.expanded"
    mkdir "$nested_dir" || fail "cannot create nested extraction directory" "$nested"
    case "$nested" in
      *.tar.gz|*.tgz|*.tar.xz)
        tar -C "$nested_dir" -xf "$nested" || fail "cannot extract nested archive" "$nested" ;;
      *)
        (cd "$nested_dir" && "${CMAKE:-cmake}" -E tar xf "$nested") || fail "cannot extract nested archive" "$nested" ;;
    esac
    expand_nested "$nested_dir" "$((depth + 1))"
  done
}
expand_nested "$work_root/extract" 0

scan_path "file://$repo_root" "repository file URL"
scan_path "file://${HOME:-}" "home file URL"
scan_path "$repo_root" "repository path"
scan_path "${HOME:-}" "home path"
scan_path "$repo_root/.cache" "dependency cache path"
scan_path "$repo_root/build" "build path"
scan_path "/tmp"/luarocks "package-manager temporary path"
if [ -n "${TMPDIR:-}" ]; then
  scan_path "${TMPDIR:-}/luarocks" "package-manager temporary path"
fi

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

is_darwin_vectis_binary() {
  file=$1
  case "$file" in
    */vectis-"$version"-*-apple-darwin/bin/vectis) return 0 ;;
  esac
  return 1
}

verify_darwin_vectis_binary_dependencies() {
  file=$1
  loads=$2

  printf '%s\n' "$loads" | sed '1d' |
  while IFS= read -r line; do
    path=$(printf '%s\n' "$line" | awk '{print $1}')
    [ -n "$path" ] || continue
    if ! allowed_darwin_absolute_path "$path"; then
      fail "Darwin vectis binary depends on a non-system dylib" "$file: $path" "static-linkage"
    fi
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
      if is_darwin_vectis_binary "$file"; then
        verify_darwin_vectis_binary_dependencies "$file" "$loads"
      fi
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
  while IFS= read -r dependency; do
    case "$dependency" in
      */*) fail "non-relocatable ELF dependency path" "$file: $dependency" ;;
    esac
  done < <(printf '%s\n' "$dynamic" | sed -n '/(NEEDED)/s/.*\[\(.*\)\].*/\1/p')
}

verify_elf_interpreter() {
  local file=$1 headers interpreter
  headers=$(readelf -l "$file" 2>/dev/null) || fail "cannot inspect ELF program headers" "$file"
  if ! printf '%s\n' "$headers" | grep -Eq '^[[:space:]]*INTERP[[:space:]]'; then
    return 0
  fi
  interpreter=$(printf '%s\n' "$headers" | sed -n 's/.*\[Requesting program interpreter: \(.*\)\]/\1/p')
  case "$interpreter" in
    /lib64/ld-linux-x86-64.so.2|/lib/ld-linux-aarch64.so.1|/lib/ld-linux-armhf.so.3|/lib/ld-musl-x86_64.so.1|/lib/ld-musl-aarch64.so.1|/lib/ld-musl-armhf.so.1) ;;
    *) fail "non-system ELF interpreter" "$file: $interpreter" ;;
  esac
}

is_linux_vectis_binary() {
  file=$1
  case "$file" in
    */vectis-"$version"-*-linux-*/bin/vectis) return 0 ;;
  esac
  return 1
}

verify_linux_vectis_static() {
  file=$1
  dynamic=$2

  if printf '%s\n' "$dynamic" | grep -E '\(NEEDED\)' >/dev/null 2>&1; then
    fail "Linux vectis binary is dynamically linked" "$file" "static-linkage"
  fi
  if readelf -l "$file" 2>/dev/null | grep -E '^[[:space:]]*INTERP[[:space:]]' >/dev/null 2>&1; then
    fail "Linux vectis binary has an ELF interpreter" "$file" "static-linkage"
  fi
}

command -v readelf >/dev/null 2>&1 || fail "ELF inspector unavailable" "readelf" "external-tool-unavailable"
find "$work_root/extract" -type f -print |
while IFS= read -r file; do
  if readelf -h "$file" >/dev/null 2>&1; then
    dynamic=$(readelf -d "$file" 2>/dev/null) || fail "cannot inspect ELF dynamic metadata" "$file"
    verify_elf_runtime_paths "$file" "$dynamic"
    if is_linux_vectis_binary "$file"; then
      verify_linux_vectis_static "$file" "$dynamic"
    fi
    verify_elf_interpreter "$file"
  elif [ "$(od -An -tx1 -N4 "$file" | tr -d ' \n')" = 7f454c46 ]; then
    fail "cannot inspect ELF header" "$file"
  fi
done

find "$work_root/extract" -type d -name "vectis-$version-arm64-apple-darwin" -print |
while IFS= read -r package_root; do
  verify_darwin_root "$package_root" arm64-apple-darwin
done

echo "release privacy ok"
