#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${VECTIS_VERSION:-$("$script_dir/release_version.sh")}
dist_dir=${VECTIS_DIST_DIR:-"$repo_root/dist"}
checksums="$dist_dir/vectis-$version-CHECKSUMS"
work_root="$repo_root/build/release-artifact-verify"

fail() {
  reason=$1
  artifact=${2:-}
  echo "$reason${artifact:+: $artifact}" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=verify-release-archives
phase=artifact-verification
status=failed
class=package-layout
reason=$reason
artifact=$artifact
next=regenerate artifacts with make package or make release-matrix, then rerun package-verify
PKT_DIAGNOSTIC_END
EOF
  exit 1
}

if [ ! -f "$checksums" ]; then
  fail "missing checksum manifest" "$checksums"
fi

manifest_count=$(find "$dist_dir" -maxdepth 1 -type f -name 'vectis-*-CHECKSUMS' | wc -l | tr -d ' ')
if [ "$manifest_count" != "1" ]; then
  fail "expected exactly one checksum manifest under dist" "$dist_dir"
fi

(cd "$dist_dir" && sha256sum -c "$(basename "$checksums")")

rm -rf "$work_root"
mkdir -p "$work_root"

manifest_files="$work_root/manifest-files.txt"
awk '{print $2}' "$checksums" | sort >"$manifest_files"

while IFS= read -r artifact_name; do
  artifact="$dist_dir/$artifact_name"
  [ -f "$artifact" ] || fail "checksum-listed artifact missing" "$artifact_name"
  case "$artifact_name" in
    vectis-"$version".tar.gz|vectis-"$version"-*.tar.gz)
      extract_dir="$work_root/${artifact_name%.tar.gz}"
      mkdir -p "$extract_dir"
      tar -C "$extract_dir" -xzf "$artifact"
      root_count=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')
      [ "$root_count" = "1" ] || fail "archive must contain exactly one root directory" "$artifact_name"
      root=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d | sort | sed -n '1p')
      root_name=$(basename "$root")
      case "$artifact_name" in
        vectis-"$version".tar.gz)
          [ "$root_name" = "vectis-$version" ] || fail "source archive root mismatch" "$artifact_name"
          [ -f "$root/VERSION" ] || fail "source archive missing VERSION" "$artifact_name"
          [ -f "$root/RELEASE_MANIFEST" ] || fail "source archive missing RELEASE_MANIFEST" "$artifact_name"
          ;;
        *)
          case "$root_name" in
            vectis-"$version"-*) ;;
            *) fail "binary SDK archive root mismatch" "$artifact_name" ;;
          esac
          [ -d "$root/include" ] || fail "binary SDK missing include/" "$artifact_name"
          [ -d "$root/lib" ] || fail "binary SDK missing lib/" "$artifact_name"
          [ -d "$root/share/doc/vectis" ] || fail "binary SDK missing share/doc/vectis" "$artifact_name"
          [ -f "$root/share/doc/vectis/LICENSE" ] || fail "binary SDK missing license" "$artifact_name"
          [ -f "$root/share/doc/vectis/README.md" ] || fail "binary SDK missing README" "$artifact_name"
          [ -f "$root/lib/cmake/vectis/vectisConfig.cmake" ] || fail "binary SDK missing CMake config" "$artifact_name"
          [ -f "$root/lib/cmake/vectis/vectisConfigVersion.cmake" ] || fail "binary SDK missing CMake version config" "$artifact_name"
          [ -f "$root/lib/pkgconfig/vectis.pc" ] || fail "binary SDK missing pkg-config metadata" "$artifact_name"
          [ -f "$root/include/vectis/vectis_version.h" ] || fail "binary SDK missing generated version header" "$artifact_name"
          grep -F "#define VECTIS_VERSION \"$version\"" "$root/include/vectis/vectis_version.h" >/dev/null ||
            fail "binary SDK version header mismatch" "$artifact_name"
          if find "$root/share" -mindepth 1 -maxdepth 1 -type d \
             \( -name '*-source' -o -name '*lua*source*' \) | grep . >/dev/null; then
            fail "binary SDK contains dependency source tree" "$artifact_name"
          fi
          if find "$root/share" -type f \
             \( -name '*.rockspec' -o -name '*.rockspec.in' -o -name '*_lua.c' \) | grep . >/dev/null; then
            fail "binary SDK contains Lua package or binding source" "$artifact_name"
          fi
          if find "$root" \( \
               -type d \( -name '.luarocks' -o -name 'luarocks' \) -o \
               -type f \( -name '*.rock' -o -name '*.rockspec' -o -name '*.rockspec.in' \) \
             \) | grep . >/dev/null; then
            fail "binary SDK contains LuaRocks artifacts" "$artifact_name"
          fi
          if [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "x86_64" ] &&
             [ "$root_name" = "vectis-$version-x86_64-linux-gnu" ]; then
            if [ -f "$root/lib/libvectis.a" ]; then
              bash "$script_dir/verify_installed_sdk.sh" "$root" static
            fi
            if [ -f "$root/lib/libvectis.so" ]; then
              bash "$script_dir/verify_installed_sdk.sh" "$root" shared
            fi
            if [ -x "$root/bin/vectis" ]; then
              LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                "$root/bin/vectis" --version >/dev/null
            fi
          fi
          ;;
      esac
      ;;
    vectis-"$version"-*.zip)
      extract_dir="$work_root/${artifact_name%.zip}"
      mkdir -p "$extract_dir"
      if command -v unzip >/dev/null 2>&1; then
        unzip -q "$artifact" -d "$extract_dir"
      else
        (cd "$extract_dir" && "${CMAKE:-cmake}" -E tar xf "$artifact")
      fi
      root_count=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')
      [ "$root_count" = "1" ] || fail "zip artifact must contain exactly one root directory" "$artifact_name"
      ;;
    *)
      fail "unexpected checksum-listed artifact name" "$artifact_name"
      ;;
  esac
done <"$manifest_files"

find "$dist_dir" -maxdepth 1 -type f \( -name 'vectis-*.tar.gz' -o -name 'vectis-*.zip' \) \
  -printf '%f\n' | sort >"$work_root/dist-release-files.txt"
if ! cmp -s "$manifest_files" "$work_root/dist-release-files.txt"; then
  echo "dist artifacts do not match checksum manifest" >&2
  diff -u "$manifest_files" "$work_root/dist-release-files.txt" || true
  fail "checksum manifest is not the release artifact set" "$checksums"
fi

echo "release artifacts ok"
