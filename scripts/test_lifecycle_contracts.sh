#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version_path="$repo_root/VERSION"
version_work="$repo_root/build/test-release-version"
source_stage_dist="$version_work/source-dist"
untracked_source_probe="$repo_root/vectis-untracked-source-probe.txt"
saved_version=
had_version=0

cleanup() {
  if [ "$had_version" -eq 1 ]; then
    printf '%s' "$saved_version" >"$version_path"
  else
    rm -f "$version_path"
  fi
  rm -f "$untracked_source_probe"
  rm -rf "$version_work"
}
trap cleanup EXIT INT TERM

assert_contains() {
  file=$1
  pattern=$2
  if ! grep -Eq "$pattern" "$file"; then
    echo "missing lifecycle contract pattern in $file: $pattern" >&2
    exit 1
  fi
}

assert_host_debug_target() {
  host_system=$1
  host_machine=$2
  expected_target=$3
  expected_processor=$4
  output=$(
    VECTIS_DEPS_DRY_RUN=1 \
      VECTIS_HOST_UNAME_S="$host_system" \
      VECTIS_HOST_UNAME_M="$host_machine" \
      "$repo_root/scripts/deps.sh" deps-host-debug
  )

  if ! printf '%s\n' "$output" | grep -Eq "^target_id=$expected_target$"; then
    echo "deps-host-debug selected wrong target for $host_system $host_machine" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  if ! printf '%s\n' "$output" |
    grep -Eq "^target_cmake_system_processor=$expected_processor$"; then
    echo "deps-host-debug selected wrong processor for $host_system $host_machine" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
}

if [ -f "$version_path" ]; then
  had_version=1
  saved_version=$(cat "$version_path")
fi

assert_contains "$repo_root/.gitignore" '^/VERSION$'
assert_contains "$repo_root/scripts/package.sh" 'target_toolchain_available\.sh'
assert_contains "$repo_root/CMakeLists.txt" 'generated/pkgconfig/vectis\.pc'
assert_contains "$repo_root/CMakeLists.txt" 'pkgconfig"\)'
assert_contains "$repo_root/cmake/vectis.pc.in" '^Name: vectis$'
assert_contains "$repo_root/cmake/package_archive.cmake" 'share/c\.pkt\.systems'
assert_contains "$repo_root/cmake/package_archive.cmake" 'CMAKE_INSTALL_NAME_TOOL'
assert_contains "$repo_root/cmake/package_archive.cmake" '@rpath/\$\{vectis_darwin_dep_name\}'
assert_contains "$repo_root/cmake/package_darwin_smoke_bundle.cmake" '@executable_path/\.\./lib'
assert_contains "$repo_root/scripts/verify_installed_sdk.sh" 'pkg-config --static --cflags --libs vectis'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK missing pkg-config metadata'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK contains dependency source tree'
assert_contains "$repo_root/tests/CMakeLists.txt" 'LABELS "lua;smoke;local"'
assert_contains "$repo_root/CMakeLists.txt" 'target_compile_options\(\$\{target\} PRIVATE'
assert_contains "$repo_root/CMakeLists.txt" 'Werror'
assert_contains "$repo_root/CMakeLists.txt" '_DARWIN_C_SOURCE'
assert_contains "$repo_root/examples/CMakeLists.txt" 'target_compile_options\(\$\{target_name\} PRIVATE'
assert_contains "$repo_root/examples/CMakeLists.txt" 'Werror'

if ! "$repo_root/scripts/target_toolchain_available.sh" x86_64-linux-gnu >/dev/null 2>&1; then
  echo "host x86_64-linux-gnu toolchain availability check failed" >&2
  exit 1
fi

assert_host_debug_target Linux x86_64 x86_64-linux-gnu x86_64
assert_host_debug_target Linux aarch64 aarch64-linux-gnu aarch64
assert_host_debug_target Linux armv7l armhf-linux-gnu arm
if VECTIS_DEPS_DRY_RUN=1 \
  VECTIS_HOST_UNAME_S=Darwin \
  VECTIS_HOST_UNAME_M=arm64 \
  "$repo_root/scripts/deps.sh" deps-host-debug >/dev/null 2>&1; then
  echo "deps-host-debug accepted unsupported Darwin host" >&2
  exit 1
fi

override_version=$(VECTIS_VERSION_OVERRIDE=1.2.3 "$repo_root/scripts/release_version.sh")
if [ "$override_version" != "1.2.3" ]; then
  echo "VECTIS_VERSION_OVERRIDE did not drive release version" >&2
  exit 1
fi

printf '%s\n' '98.76.54' >"$version_path"
git_version=$("$repo_root/scripts/release_version.sh")
if [ "$git_version" = "98.76.54" ]; then
  echo "git worktree version detection read ignored /VERSION" >&2
  exit 1
fi

rm -rf "$version_work"
mkdir -p "$version_work/scripts"
cp "$repo_root/scripts/release_version.sh" "$version_work/scripts/release_version.sh"
git -C "$version_work" init -q
git -C "$version_work" config user.email lifecycle@example.invalid
git -C "$version_work" config user.name "Lifecycle Test"
git -C "$version_work" add scripts/release_version.sh
git -C "$version_work" commit -q -m 'test: seed version worktree'
git -C "$version_work" tag v1.2.3
tagged_override=$(VECTIS_VERSION_OVERRIDE=9.9.9 "$version_work/scripts/release_version.sh")
if [ "$tagged_override" != "1.2.3" ]; then
  echo "exact lightweight HEAD tag did not take precedence over override" >&2
  exit 1
fi
git -C "$version_work" tag -d v1.2.3 >/dev/null
untagged_override=$(VECTIS_VERSION_OVERRIDE=9.9.9 "$version_work/scripts/release_version.sh")
if [ "$untagged_override" != "9.9.9" ]; then
  echo "untagged git worktree did not accept explicit override" >&2
  exit 1
fi
printf '%s\n' '7.7.7' >"$version_work/VERSION"
untagged_version=$("$version_work/scripts/release_version.sh")
if [ "$untagged_version" != "0.0.0" ]; then
  echo "untagged git worktree did not resolve to 0.0.0" >&2
  exit 1
fi
git -C "$version_work" tag -a v2.0.0 -m 'annotated test tag'
annotated_version=$("$version_work/scripts/release_version.sh")
if [ "$annotated_version" != "0.0.0" ]; then
  echo "annotated HEAD tag was accepted as release version" >&2
  exit 1
fi

printf '%s\n' 'must not ship' >"$untracked_source_probe"
mkdir -p "$source_stage_dist"
source_archive=$(VECTIS_DIST_DIR="$source_stage_dist" "$repo_root/scripts/stage_release_sources.sh")
if tar -tzf "$source_archive" | grep -Eq '/vectis-untracked-source-probe\.txt$'; then
  echo "source archive included a non-ignored untracked worktree file" >&2
  exit 1
fi
if tar -xOzf "$source_archive" "vectis-$("$repo_root/scripts/release_version.sh")/RELEASE_MANIFEST" |
   grep -Eq '^vectis-untracked-source-probe\.txt$'; then
  echo "source archive manifest included a non-ignored untracked worktree file" >&2
  exit 1
fi

for preset in \
  x86_64-linux-gnu \
  x86_64-linux-musl \
  aarch64-linux-gnu \
  aarch64-linux-musl \
  armhf-linux-gnu \
  armhf-linux-musl \
  arm64-apple-darwin
do
  assert_contains "$repo_root/CMakePresets.json" "\"VECTIS_TARGET_ID\":[[:space:]]*\"$preset\""
done

for target in \
  print-release-version \
  asan \
  fuzz-smoke \
  package-source \
  package-source-smoke \
  package-checksums \
  package-verify \
  verify-release-archives \
  verify-release-privacy \
  release-darwin-smoke-bundle \
  release-matrix \
  prerelease-hardening \
  release \
  finalize-slice \
  prerelease \
  lua-test \
  lua-env \
  clean-dist
do
  assert_contains "$repo_root/Makefile" "^$target:"
done

echo "lifecycle contracts ok"
