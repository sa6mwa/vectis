#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version_path="$repo_root/VERSION"
version_work="$repo_root/build/test-release-version"
source_stage_dist="$version_work/source-dist"
luarocks_dist="$version_work/luarocks-dist"
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

assert_no_landed_test_assets() {
  if grep -RInE --exclude='test_lifecycle_contracts.sh' \
    '(\.\./landed|\blanded\b|\bLanded\b)' \
    "$repo_root/scripts" \
    "$repo_root/tests" \
    "$repo_root/examples" \
    "$repo_root/src" \
    "$repo_root/include" \
    "$repo_root/CMakeLists.txt"; then
    echo "runtime code and executable fixtures must not depend on Landed assets" >&2
    exit 1
  fi
}

assert_action_surface_contract() {
  if grep -RInE --exclude='test_lifecycle_contracts.sh' \
    '(admin-operation|--admin-operation|--subcommand|pack[ _-]v2|pack[ _-]V2|Pack V2|VECTIS_PACK_V2|VECTIS_PACK2|PACK_V2)' \
    "$repo_root/docs" \
    "$repo_root/scripts" \
    "$repo_root/tests" \
    "$repo_root/examples" \
    "$repo_root/src" \
    "$repo_root/include" \
    "$repo_root/CMakeLists.txt" \
    "$repo_root/Makefile"; then
    echo "vectis command surface must stay on -a/--action with no pack V2 or admin-operation aliases" >&2
    exit 1
  fi
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis -a\|--action pack'
  assert_contains "$repo_root/src/vectis_cli.c" 'traces Lua line execution to stderr'
}

assert_kore_lonejson_contract() {
  assert_contains "$repo_root/CMakeLists.txt" 'src/ljson\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'KORE_USE_LONEJSON'
  assert_contains "$repo_root/vendor/kore/upstream/src/ljson.c" 'lonejson_validate_reader'
  assert_contains "$repo_root/vendor/kore/upstream/src/ljson.c" 'lonejson_parse_reader'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_directory_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_order_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_auth_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_badreq_map'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'S_SRC\+=src/ljson\.c'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'CFLAGS\+=-DKORE_USE_LONEJSON'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'LDFLAGS\+=-L\$\(LONEJSON_PATH\)/lib -llonejson'
}

assert_lockdc_lua_runtime_contract() {
  assert_contains "$repo_root/CMakeLists.txt" 'lua/vectis/lockd\.lua'
  assert_contains "$repo_root/CMakeLists.txt" 'share/lockdc-source/src/lua/lockdc_lua\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'share/lockdc-source/lua/lockdc/init\.lua'
  assert_contains "$repo_root/CMakeLists.txt" 'add_library\(vectis_lockdc_lua OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" '\$<TARGET_OBJECTS:vectis_lockdc_lua>'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "lockdc\.core"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "lockdc", vectis_lockdc_lua_init'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.lockd", vectis_lockd_lua_init'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("lockdc"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.lockd"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'lockdc\.version_string'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'client_bundle == "embedded"'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'embedded_lockd_bundle_source'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M.with_client'
  assert_contains "$repo_root/examples/lua/lockd_state.lua" 'vectis\.lockd\.open'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.open'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.raw\.encode_json'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'lockdc\.open'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'vectis\.lockd\.open'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'client_bundle_source = assert\(vectis\.embedded_lockd_bundle_source\(\)\)'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'server:consumer_service'
}

assert_luarocks_artifact_rejected() {
  dist=$luarocks_dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=vectis-$version-x86_64-linux-musl.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p \
    "$root/include/vectis" \
    "$root/lib/cmake/vectis" \
    "$root/lib/pkgconfig" \
    "$root/share/doc/vectis" \
    "$root/.luarocks"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
  printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
  printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
  printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  printf '%s\n' 'must not ship' >"$root/.luarocks/bad.rock"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/verify.out" 2>"$dist/verify.err"; then
    echo "release artifact verifier accepted LuaRocks artifacts" >&2
    exit 1
  fi
  if ! grep -Fq "binary SDK contains LuaRocks artifacts" "$dist/verify.err"; then
    echo "release artifact verifier rejected LuaRocks fixture for the wrong reason" >&2
    cat "$dist/verify.err" >&2
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
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK contains LuaRocks artifacts'
assert_contains "$repo_root/tests/CMakeLists.txt" 'LABELS "lua;smoke;local"'
assert_contains "$repo_root/CMakeLists.txt" 'target_compile_options\(\$\{target\} PRIVATE'
assert_contains "$repo_root/CMakeLists.txt" 'Werror'
assert_contains "$repo_root/CMakeLists.txt" '_DARWIN_C_SOURCE'
assert_contains "$repo_root/CMakeLists.txt" 'libpid0_enabled "0"'
assert_contains "$repo_root/scripts/deps.sh" 'libpid0_enabled=\$pid0_enabled'
assert_contains "$repo_root/examples/CMakeLists.txt" 'target_compile_options\(\$\{target_name\} PRIVATE'
assert_contains "$repo_root/examples/CMakeLists.txt" 'Werror'
assert_no_landed_test_assets
assert_action_surface_contract
assert_kore_lonejson_contract
assert_lockdc_lua_runtime_contract
assert_luarocks_artifact_rejected

if ! "$repo_root/scripts/target_toolchain_available.sh" x86_64-linux-gnu >/dev/null 2>&1; then
  echo "host x86_64-linux-gnu toolchain availability check failed" >&2
  exit 1
fi

assert_host_debug_target Linux x86_64 x86_64-linux-gnu x86_64
assert_host_debug_target Linux aarch64 aarch64-linux-gnu aarch64
assert_host_debug_target Linux armv7l armhf-linux-gnu arm
linux_deps_output=$(
  VECTIS_DEPS_DRY_RUN=1 "$repo_root/scripts/deps.sh" deps-x86_64-linux-gnu
)
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^libpid0_enabled=1$'; then
  echo "Linux dependency preset did not enable libpid0" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^libpid0_version=0\.4\.2$'; then
  echo "Linux dependency preset did not pin libpid0 0.4.2" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
for expected in \
  '^system_sha256=0bbb1cbaf60b0a94fb5a6b3756123088b45e2bef9e38079038f22e3c07febb2e$' \
  '^liblockdc_sha256=ef73caef7f06e629d90495304ed263541e01ed5d8785410b45af8424cc2d90fb$' \
  '^lonejson_sha256=e04f80b907d92f7e38f825fbd339297e85372fc1ce110abb9a93715ee450ece3$' \
  '^pslog_sha256=7981ce7e60f6f1e144042e7a9192bb661472756ae34336fb0c2ed8316b31945f$' \
  '^cai_sha256=e344102fa5b46e8c05d67a5120ea0c74bf9ee8ad9ec0bc01e08ea5ccc1f1bdc9$' \
  '^lql_sha256=a32b3ecc33b0634df23c630843b1c2c16a8a2caa947109a33bad20965e47a399$' \
  '^softline_sha256=5d5e662269cf5bae9276f1ba7216dfb7e63127ea89c7c9b5f23cb33dbc970012$'
do
  if ! printf '%s\n' "$linux_deps_output" | grep -Eq "$expected"; then
    echo "Linux dependency preset did not expose expected upgraded dependency pin: $expected" >&2
    printf '%s\n' "$linux_deps_output" >&2
    exit 1
  fi
done
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^pslog_sha256='; then
  echo "Linux dependency preset did not expose libpslog metadata" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^softline_sha256='; then
  echo "Linux dependency preset did not expose softline metadata" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
darwin_deps_output=$(
  VECTIS_DEPS_DRY_RUN=1 "$repo_root/scripts/deps.sh" deps-arm64-apple-darwin
)
if ! printf '%s\n' "$darwin_deps_output" | grep -Eq '^libpid0_enabled=0$'; then
  echo "Darwin dependency preset did not disable libpid0" >&2
  printf '%s\n' "$darwin_deps_output" >&2
  exit 1
fi
if printf '%s\n' "$darwin_deps_output" | grep -Eq '^libpid0_version='; then
  echo "Darwin dependency preset exposed libpid0 version metadata" >&2
  printf '%s\n' "$darwin_deps_output" >&2
  exit 1
fi
if ! printf '%s\n' "$darwin_deps_output" | grep -Eq '^softline_sha256='; then
  echo "Darwin dependency preset did not expose softline metadata" >&2
  printf '%s\n' "$darwin_deps_output" >&2
  exit 1
fi
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
  prerelease-live \
  prerelease-hardening \
  release \
  finalize-slice \
  prerelease \
  lua-test \
  lua-env \
  clean-dist \
  valgrind \
  lifecycle-version-contract \
  test-cpkt-toolchains
do
  assert_contains "$repo_root/Makefile" "^$target:"
done
assert_contains "$repo_root/Makefile" 'VECTIS_LIVE_OAUTH2_ENABLE=1'
assert_contains "$repo_root/scripts/test-live-oauth2.sh" 'VECTIS_LIVE_OAUTH2_ENABLE'
assert_contains "$repo_root/scripts/test-live-oauth2.sh" 'SKIP: set VECTIS_LIVE_OAUTH2_ENABLE=1'

assert_contains "$repo_root/cmake/toolchains/x86_64-linux-gnu.cmake" 'cpkt_configure_bootlin_toolchain\(x86_64-linux-gnu\)'
assert_contains "$repo_root/cmake/toolchains/x86_64-linux-musl.cmake" 'cpkt_configure_bootlin_toolchain\(x86_64-linux-musl\)'
assert_contains "$repo_root/cmake/toolchains/aarch64-linux-gnu.cmake" 'cpkt_configure_bootlin_toolchain\(aarch64-linux-gnu\)'
assert_contains "$repo_root/cmake/toolchains/aarch64-linux-musl.cmake" 'cpkt_configure_bootlin_toolchain\(aarch64-linux-musl\)'
assert_contains "$repo_root/cmake/toolchains/armhf-linux-gnu.cmake" 'cpkt_configure_bootlin_toolchain\(armhf-linux-gnu\)'
assert_contains "$repo_root/cmake/toolchains/armhf-linux-musl.cmake" 'cpkt_configure_bootlin_toolchain\(armhf-linux-musl\)'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(CpktOpcUa CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(CpktSus CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(CpktAudio CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(liblql CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'OBJECT_DEPENDS "\$\{vectis_dependency_manifest\}"'
assert_contains "$repo_root/cmake/vectis.pc.in" 'cpkt-opcua cpkt-sus cpkt-audio'
assert_contains "$repo_root/cmake/vectis.pc.in" 'liblql'

echo "lifecycle contracts ok"
