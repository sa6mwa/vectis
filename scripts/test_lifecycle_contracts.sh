#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version_path="$repo_root/VERSION"
version_work="$repo_root/build/test-release-version"
source_stage_dist="$version_work/source-dist"
luarocks_dist="$version_work/luarocks-dist"
untracked_source_probe="$repo_root/vectis-untracked-source-probe.txt"
saved_version=
had_version=0
if [ -f "$version_path" ]; then
  had_version=1
  saved_version=$(cat "$version_path")
fi

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
  if ! grep -Eq -- "$pattern" "$file"; then
    echo "missing lifecycle contract pattern in $file: $pattern" >&2
    exit 1
  fi
}

assert_not_contains() {
  file=$1
  pattern=$2
  if grep -Eq -- "$pattern" "$file"; then
    echo "forbidden lifecycle contract pattern in $file: $pattern" >&2
    exit 1
  fi
}

# This suite checks actual lifecycle behavior. API behavior belongs to CTest,
# and semantic facade/release review belongs to the responsible agent.

port_exports=$(VECTIS_SSH_PORT=61000 "$repo_root/scripts/devenv.sh" env)
case "$port_exports" in
  *'export VECTIS_SSH_PORT=61000'*) ;;
  *) echo 'devenv port override was ignored' >&2; exit 1 ;;
esac
if VECTIS_SSH_PORT=0 "$repo_root/scripts/devenv.sh" env >/dev/null 2>&1; then
  echo 'devenv accepted a privileged port' >&2
  exit 1
fi
if VECTIS_SSH_PORT=61000 VECTIS_MQTT_PORT=61000 \
   "$repo_root/scripts/devenv.sh" env >/dev/null 2>&1; then
  echo 'devenv accepted duplicate service ports' >&2
  exit 1
fi
mkdir -p "$version_work/podman-bin"
cat > "$version_work/podman-bin/podman" <<'PODMAN'
#!/usr/bin/env bash
case "${1:-}" in
  info) printf 'true\n' ;;
  pod) [ "${2:-}" = exists ] || exit 2; exit 1 ;;
  ps)
    [ "${2:-}" = -a ] || exit 2
    if [ "${MOCK_PODMAN_PS_FAIL:-0}" = 1 ]; then exit 125; fi
    if [ -n "${MOCK_PODMAN_CONTAINER:-}" ]; then
      printf '%s\n' "$MOCK_PODMAN_CONTAINER"
    fi
    ;;
  *) exit 2 ;;
esac
PODMAN
chmod +x "$version_work/podman-bin/podman"
devenv_name="vectis-$(printf %s "$repo_root" | sha256sum | cut -c1-10)"
fake_podman_path="$version_work/podman-bin:$PATH"
PATH="$fake_podman_path" MOCK_PODMAN_CONTAINER=other-project-container \
  "$repo_root/scripts/devenv.sh" down
if PATH="$fake_podman_path" \
   MOCK_PODMAN_CONTAINER="$devenv_name-minio-minio" \
   "$repo_root/scripts/devenv.sh" down >"$version_work/devenv-down.log" 2>&1; then
  echo 'devenv accepted an orphan container after pod removal' >&2
  exit 1
fi
assert_contains "$version_work/devenv-down.log" \
  "Podman container $devenv_name-minio-minio still exists"
if PATH="$fake_podman_path" MOCK_PODMAN_PS_FAIL=1 \
   "$repo_root/scripts/devenv.sh" down >"$version_work/devenv-down.log" 2>&1; then
  echo 'devenv accepted a failed container inspection' >&2
  exit 1
fi
assert_contains "$version_work/devenv-down.log" \
  'Failed to inspect Vectis containers after devenv down'
assert_luarocks_artifact_rejected() {
  dist=$luarocks_dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=vectis-$version-x86_64-linux-musl.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
	mkdir -p "$dist/fakebin"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
	mkdir -p \
	  "$root/bin" \
	  "$root/include/vectis" \
	  "$root/lib/cmake/vectis" \
	  "$root/lib/pkgconfig" \
	  "$root/deps" \
	  "$root/share/doc/vectis" \
	  "$root/.luarocks"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
	printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
	printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
	write_fixture_provider_manifests "$root" x86_64-linux-musl
	printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  printf '%s\n' 'must not ship' >"$root/.luarocks/bad.rock"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_RELEASE_BUILD_ROOT="$dist/no-build" \
       VECTIS_READELF="$dist/fakebin/readelf" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
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

assert_linux_release_target_payload_checked() {
  dist=$version_work/target-payload-dist
  version=0.0.0
  root_name=vectis-$version-aarch64-linux-gnu
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p "$dist/fakebin"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
	mkdir -p \
	  "$root/bin" \
	  "$root/include/vectis" \
	  "$root/lib/cmake/vectis" \
	  "$root/lib/pkgconfig" \
	  "$root/deps" \
	  "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
	printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
	printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
	write_fixture_provider_manifests "$root" aarch64-linux-gnu
	printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_RELEASE_BUILD_ROOT="$dist/no-build" \
       VECTIS_READELF="$dist/fakebin/readelf" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/target.out" 2>"$dist/target.err"; then
    echo "release artifact verifier accepted a mislabeled Linux SDK payload" >&2
    exit 1
  fi
  if ! grep -Fq "Linux binary SDK ELF machine target mismatch" "$dist/target.err"; then
    echo "release artifact verifier rejected mislabeled Linux SDK payload for the wrong reason" >&2
    cat "$dist/target.err" >&2
    exit 1
  fi
}

assert_linux_release_manifest_target_checked() {
  dist=$version_work/target-manifest-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p "$root/deps"
  write_fixture_provider_manifests "$root" x86_64-linux-gnu
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/manifest.out" 2>"$dist/manifest.err"; then
    echo "release artifact verifier accepted a mislabeled provider target manifest" >&2
    exit 1
  fi
  if ! grep -Fq "SDK target/owner mismatch" "$dist/manifest.err"; then
    echo "release artifact verifier rejected mislabeled provider manifest for the wrong reason" >&2
    cat "$dist/manifest.err" >&2
    exit 1
  fi
}

assert_linux_release_binary_static_checked() {
  dist=$version_work/static-binary-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p "$dist/fakebin"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  -d:*/bin/vectis)
    printf '%s\n' ' 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]'
    ;;
  -l:*/bin/vectis)
    printf '%s\n' '  INTERP         0x0000000000000318 0x0000000000400318 0x0000000000400318'
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
	mkdir -p \
	  "$root/bin" \
	  "$root/include/vectis" \
	  "$root/lib/cmake/vectis" \
	  "$root/lib/pkgconfig" \
	  "$root/deps" \
	  "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
	printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
	printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
	write_fixture_provider_manifests "$root" x86_64-linux-musl
	printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_RELEASE_BUILD_ROOT="$dist/no-build" \
       VECTIS_READELF="$dist/fakebin/readelf" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/static.out" 2>"$dist/static.err"; then
    echo "release artifact verifier accepted a dynamically linked Linux vectis binary" >&2
    exit 1
  fi
  if ! grep -Fq "Linux vectis binary is dynamically linked" "$dist/static.err"; then
    echo "release artifact verifier rejected dynamic Linux vectis binary for the wrong reason" >&2
    cat "$dist/static.err" >&2
    exit 1
  fi
}

assert_linux_release_readelf_discovered_from_target_cache() {
  dist=$version_work/readelf-cache-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=$root_name.tar.gz
  root="$dist/$root_name"
  build_dir="$dist/build/x86_64-linux-musl-release"

  rm -rf "$dist"
  mkdir -p "$dist/fakebin" "$build_dir"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  -d:*/bin/vectis)
    exit 1
    ;;
  -l:*/bin/vectis)
    exit 1
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
  printf '%s\n' "CMAKE_READELF:FILEPATH=$dist/fakebin/readelf" >"$build_dir/CMakeCache.txt"
  mkdir -p \
    "$root/bin" \
    "$root/include/vectis" \
    "$root/lib/cmake/vectis" \
    "$root/lib/pkgconfig" \
    "$root/deps" \
    "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
  printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
  printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
  write_fixture_provider_manifests "$root" x86_64-linux-musl
  printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if ! VECTIS_RELEASE_BUILD_ROOT="$dist/build" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/readelf.out" 2>"$dist/readelf.err"; then
    echo "release artifact verifier did not use target-cache readelf" >&2
    cat "$dist/readelf.err" >&2
    exit 1
  fi
}

assert_linux_release_matrix_required() {
  dist=$version_work/matrix-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-gnu
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p \
    "$root/bin" \
    "$root/include/vectis" \
    "$root/lib/cmake/vectis" \
    "$root/lib/pkgconfig" \
    "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
  printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
  printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
  printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_REQUIRE_LINUX_RELEASE_MATRIX=1 \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/matrix.out" 2>"$dist/matrix.err"; then
    echo "release artifact verifier accepted an incomplete Linux release matrix" >&2
    exit 1
  fi
  if ! grep -Fq "missing required Linux release artifact" "$dist/matrix.err"; then
    echo "release artifact verifier rejected incomplete Linux matrix for the wrong reason" >&2
    cat "$dist/matrix.err" >&2
    exit 1
  fi
}
if [ -f "$version_path" ]; then
  had_version=1
  saved_version=$(cat "$version_path")
fi


write_fixture_provider_manifests() {
  local root=$1 target=$2 name group version
  for pair in cpkt:core cpktdb:db cpktmisc:misc; do
    name=${pair%%:*}; group=${pair#*:}
    version=$(awk -F '\t' -v name="$name" -v target="$target" '$1=="sdk" && $2==name && $4==target {print $3;exit}' "$repo_root/scripts/dependency-pins.tsv")
    mkdir -p "$root/deps/$name/share/cpkt/packages"
    printf '{"group":"%s","target_id":"%s","release_version":"%s"}\n' \
      "$group" "$target" "$version" >"$root/deps/$name/share/cpkt/packages/$group.json"
  done
}

assert_luarocks_artifact_rejected
assert_linux_release_target_payload_checked
assert_linux_release_manifest_target_checked
assert_linux_release_binary_static_checked
assert_linux_release_readelf_discovered_from_target_cache
assert_linux_release_matrix_required

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

bash "$repo_root/scripts/test-release-tag-contract.sh"

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

# A development-branch release must fail before Make can clean generated state.
branch=$(git -C "$repo_root" symbolic-ref --short HEAD)
default=$(git -C "$repo_root" symbolic-ref --short refs/remotes/origin/HEAD)
if [[ "$branch" != "${default#origin/}" ]]; then
  sentinel="$version_work/release-must-not-clean"
  : >"$sentinel"
  if make -C "$repo_root" --no-print-directory release >"$version_work/release-guard.log" 2>&1; then
    echo 'release accepted a development branch' >&2; exit 1
  fi
  [[ -f "$sentinel" ]] || { echo 'release cleaned before checking identity' >&2; exit 1; }
  assert_contains "$version_work/release-guard.log" 'release identity:'
fi

echo "lifecycle contracts ok"
