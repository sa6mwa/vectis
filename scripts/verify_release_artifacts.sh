#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${VECTIS_VERSION:-$("$script_dir/release_version.sh")}
dist_dir=${VECTIS_DIST_DIR:-"$repo_root/dist"}
checksums="$dist_dir/vectis-$version-CHECKSUMS"
work_root="$repo_root/build/release-artifact-verify"
cmake_bin=${CMAKE:-cmake}
release_build_root=${VECTIS_RELEASE_BUILD_ROOT:-"$repo_root/build"}

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
lua_artifacts_validated=0
required_linux_targets="
x86_64-linux-gnu
x86_64-linux-musl
aarch64-linux-gnu
aarch64-linux-musl
armhf-linux-gnu
armhf-linux-musl
"

if [ "${VECTIS_REQUIRE_LINUX_RELEASE_MATRIX:-0}" != "0" ]; then
  for target_id in $required_linux_targets; do
    expected_artifact="vectis-$version-$target_id.tar.gz"
    if ! grep -Fx "$expected_artifact" "$manifest_files" >/dev/null 2>&1; then
      fail "missing required Linux release artifact" "$expected_artifact"
    fi
  done
fi

linux_target_machine_pattern() {
  case "$1" in
    x86_64-linux-*) printf '%s\n' 'Advanced Micro Devices X86-64|X86-64|x86-64' ;;
    aarch64-linux-*) printf '%s\n' 'AArch64|ARM aarch64' ;;
    armhf-linux-*) printf '%s\n' 'ARM' ;;
    *) fail "unsupported Linux target for ELF verification" "$1" ;;
  esac
}

linux_target_class_pattern() {
  case "$1" in
    armhf-linux-*) printf '%s\n' 'ELF32' ;;
    x86_64-linux-*|aarch64-linux-*) printf '%s\n' 'ELF64' ;;
    *) fail "unsupported Linux target for ELF verification" "$1" ;;
  esac
}

discover_linux_readelf() {
  local target_id=$1
  local build_dir="$release_build_root/$target_id-release"
  local tools
  local READELF

  if [ -f "$build_dir/CMakeCache.txt" ]; then
    if tools=$(unset VECTIS_READELF; "$script_dir/discover_target_tools.sh" \
        --build-dir "$build_dir" --target-id "$target_id" 2>/dev/null); then
      READELF=
      eval "$tools"
      if [ -n "${READELF:-}" ] && [ -x "$READELF" ]; then
        printf '%s\n' "$READELF"
        return 0
      fi
    fi
  fi

  if [ -n "${VECTIS_READELF:-}" ] && [ -x "$VECTIS_READELF" ]; then
    printf '%s\n' "$VECTIS_READELF"
    return 0
  fi
  if command -v "$target_id-readelf" >/dev/null 2>&1; then
    command -v "$target_id-readelf"
    return 0
  fi
  if command -v readelf >/dev/null 2>&1; then
    command -v readelf
    return 0
  fi
  return 1
}

verify_pack_runner_inputs() {
  local root=$1
  local target_id=$2
  local artifact_name=$3
  local manifest="$root/share/vectis/pack-runner-link-inputs.json"
  local runner="$root/lib/vectis/pack/libvectis_pack_runner.a"

  [ -f "$manifest" ] ||
    fail "binary SDK missing pack-runner link input manifest" "$artifact_name"
  [ -f "$runner" ] ||
    fail "binary SDK missing pack-runner archive" "$artifact_name"
  grep -F '"format": "vectis-pack-runner-link-inputs"' "$manifest" >/dev/null ||
    fail "binary SDK pack-runner manifest format mismatch" "$artifact_name"
  grep -F "\"target_id\": \"$target_id\"" "$manifest" >/dev/null ||
    fail "binary SDK pack-runner manifest target mismatch" "$artifact_name"
  grep -F '"runner_archive": "lib/vectis/pack/libvectis_pack_runner.a"' "$manifest" >/dev/null ||
    fail "binary SDK pack-runner manifest archive path mismatch" "$artifact_name"
  grep -F '"cmake_target": "vectis::pack_runner"' "$manifest" >/dev/null ||
    fail "binary SDK pack-runner manifest target name mismatch" "$artifact_name"
  grep -F '__VECTIS,__pack_header' "$manifest" >/dev/null ||
    fail "binary SDK pack-runner manifest missing Mach-O section contract" "$artifact_name"
}

verify_binary_sdk_manifest_target() {
  root=$1
  target_id=$2
  artifact_name=$3
  manifest="$root/share/c.pkt.systems/manifest.txt"

  [ -f "$manifest" ] ||
    fail "binary SDK missing c.pkt.systems target manifest" "$artifact_name"
  manifest_target=$(sed -n 's/^target_id=//p' "$manifest" | sed -n '1p')
  [ -n "$manifest_target" ] ||
    fail "binary SDK c.pkt.systems target manifest missing target_id" "$artifact_name"
  [ "$manifest_target" = "$target_id" ] ||
    fail "binary SDK c.pkt.systems target manifest mismatch" \
      "$artifact_name: $manifest_target != $target_id"
}

verify_linux_elf_header_target() {
  file=$1
  target_id=$2
  header=$3
  machine_pattern=$(linux_target_machine_pattern "$target_id")
  class_pattern=$(linux_target_class_pattern "$target_id")
  machines=$(printf '%s\n' "$header" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
  classes=$(printf '%s\n' "$header" | sed -n 's/^[[:space:]]*Class:[[:space:]]*//p')

  [ -n "$machines" ] || fail "Linux binary SDK ELF payload missing machine metadata" "$file"
  [ -n "$classes" ] || fail "Linux binary SDK ELF payload missing class metadata" "$file"
  if bad_machine=$(printf '%s\n' "$machines" | grep -Ev "$machine_pattern" | sed -n '1p') &&
     [ -n "$bad_machine" ]; then
    fail "Linux binary SDK ELF machine target mismatch" "$file: $bad_machine != $target_id"
  fi
  if bad_class=$(printf '%s\n' "$classes" | grep -Ev "$class_pattern" | sed -n '1p') &&
     [ -n "$bad_class" ]; then
    fail "Linux binary SDK ELF class target mismatch" "$file: $bad_class != $target_id"
  fi
}

verify_linux_required_elf_payload() {
  file=$1
  target_id=$2
  label=$3

  if ! header=$("$readelf_bin" -h "$file" 2>/dev/null); then
    fail "$label is not a readable ELF payload" "$file"
  fi
  verify_linux_elf_header_target "$file" "$target_id" "$header"
}

verify_linux_vectis_binary_static() {
  file=$1

  if dynamic=$("$readelf_bin" -d "$file" 2>/dev/null) &&
     printf '%s\n' "$dynamic" | grep -E '\(NEEDED\)' >/dev/null 2>&1; then
    fail "Linux vectis binary is dynamically linked" "$file"
  fi
  if "$readelf_bin" -l "$file" 2>/dev/null |
     grep -E '^[[:space:]]*INTERP[[:space:]]' >/dev/null 2>&1; then
    fail "Linux vectis binary has an ELF interpreter" "$file"
  fi
}

verify_linux_tree_elf_targets() {
  root=$1
  target_id=$2

  while IFS= read -r file; do
    if header=$("$readelf_bin" -h "$file" 2>/dev/null); then
      verify_linux_elf_header_target "$file" "$target_id" "$header"
    fi
  done < <(find "$root/bin" "$root/lib" -type f -print)
}

verify_linux_sdk_consumer_build() {
  root=$1
  target_id=$2
  link_mode=$3
  toolchain="$repo_root/cmake/toolchains/$target_id.cmake"
  build_dir="$work_root/consumer-$target_id-$link_mode"

  [ -f "$toolchain" ] ||
    fail "missing Linux target toolchain for SDK consumer smoke" "$target_id"
  "$cmake_bin" \
    -S "$repo_root/tests/install" \
    -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DVECTIS_EXTERNAL_ROOT="$root" \
    -DCMAKE_PREFIX_PATH="$root" \
    -DVECTIS_CONSUMER_LINK="$link_mode"
  "$cmake_bin" --build "$build_dir"
}

while IFS= read -r artifact_name; do
  artifact="$dist_dir/$artifact_name"
  [ -f "$artifact" ] || fail "checksum-listed artifact missing" "$artifact_name"
  case "$artifact_name" in
    vectis-lua-"$version".tar.gz)
      extract_dir="$work_root/${artifact_name%.tar.gz}"
      mkdir -p "$extract_dir"
      tar -C "$extract_dir" -xzf "$artifact"
      root="$extract_dir/vectis-lua-$version"
      [ -d "$root" ] || fail "Lua source archive root mismatch" "$artifact_name"
      [ -f "$root/VERSION" ] || fail "Lua source archive missing VERSION" "$artifact_name"
      [ -f "$root/RELEASE_MANIFEST" ] || fail "Lua source archive missing RELEASE_MANIFEST" "$artifact_name"
      [ -f "$root/vectis.rockspec.in" ] || fail "Lua source archive missing rockspec template" "$artifact_name"
      [ -f "$root/lua/vectis.lua" ] || fail "Lua source archive missing vectis.lua" "$artifact_name"
      [ -f "$root/lua/vectis/status.lua" ] || fail "Lua source archive missing vectis.status" "$artifact_name"
      if (cd "$root" &&
          find . \( -path './.git/*' -o -path './build/*' -o -path './dist/*' -o -path './.luarocks/*' \)) |
         grep . >/dev/null; then
        fail "Lua source archive contains generated or VCS state" "$artifact_name"
      fi
      ;;
    vectis-"$version"-1.rockspec|vectis-"$version"-1.src.rock)
      if [ "$lua_artifacts_validated" = "0" ]; then
        bash "$script_dir/validate_luarocks.sh" "$dist_dir" "$version" >/dev/null
        lua_artifacts_validated=1
      fi
      ;;
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
          [ "$root_name.tar.gz" = "$artifact_name" ] ||
            fail "binary SDK archive/root target mismatch" "$artifact_name"
          target_id=${root_name#"vectis-$version-"}
          case "$target_id" in
            x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl|arm64-apple-darwin) ;;
            *) fail "binary SDK target is not supported" "$artifact_name" ;;
          esac
          verify_binary_sdk_manifest_target "$root" "$target_id" "$artifact_name"
          verify_pack_runner_inputs "$root" "$target_id" "$artifact_name"
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
          case "$target_id" in
            *-linux-*)
              readelf_bin=$(discover_linux_readelf "$target_id") ||
                fail "readelf is required to verify Linux binary SDK payloads" "$artifact_name"
              [ -x "$root/bin/vectis" ] ||
                fail "Linux binary SDK missing executable vectis binary" "$artifact_name"
              [ -f "$root/lib/libvectis.a" ] ||
                fail "Linux binary SDK missing static libvectis library" "$artifact_name"
              verify_linux_required_elf_payload "$root/bin/vectis" "$target_id" \
                "Linux vectis binary"
              verify_linux_vectis_binary_static "$root/bin/vectis"
              verify_linux_required_elf_payload "$root/lib/libvectis.a" "$target_id" \
                "Linux static libvectis archive"
              verify_linux_tree_elf_targets "$root" "$target_id"
              if [ "${VECTIS_REQUIRE_LINUX_RELEASE_MATRIX:-0}" != "0" ]; then
                verify_linux_sdk_consumer_build "$root" "$target_id" static
                if [ -f "$root/lib/libvectis.so" ]; then
                  verify_linux_sdk_consumer_build "$root" "$target_id" shared
                fi
              fi
              ;;
          esac
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
              LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                bash "$script_dir/verify_vectis_lua_preloads.sh" \
                  "$root/bin/vectis" "$version" >/dev/null
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

find "$dist_dir" -maxdepth 1 -type f \( \
    -name 'vectis-*.tar.gz' -o \
    -name 'vectis-*.zip' -o \
    -name 'vectis-*.rockspec' -o \
    -name 'vectis-*.src.rock' \
  \) \
  -printf '%f\n' | sort >"$work_root/dist-release-files.txt"
if ! cmp -s "$manifest_files" "$work_root/dist-release-files.txt"; then
  echo "dist artifacts do not match checksum manifest" >&2
  diff -u "$manifest_files" "$work_root/dist-release-files.txt" || true
  fail "checksum manifest is not the release artifact set" "$checksums"
fi

echo "release artifacts ok"
