#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=9.8.7
work="$repo_root/build/test-release-privacy-contracts"
dist="$work/dist"
payload="$work/payload"

rm -rf "$work"
mkdir -p "$dist" "$payload"

cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

write_artifact() {
  content=$1
  artifact=$2

  rm -rf "$payload"
  mkdir -p "$payload/vectis-$version"
  printf '%s\n' "$content" >"$payload/vectis-$version/leak.txt"
  tar -C "$payload" -czf "$dist/$artifact" "vectis-$version"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")
}

expect_privacy_failure() {
  label=$1
  content=$2
  artifact="vectis-$version.tar.gz"

  rm -rf "$dist"
  mkdir -p "$dist"
  write_artifact "$content" "$artifact"
  if VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
     "$repo_root/scripts/verify_release_privacy.sh" >"$work/$label.out" 2>"$work/$label.err"; then
    echo "privacy verifier accepted $label leak" >&2
    exit 1
  fi
  grep -F "$label" "$work/$label.err" >/dev/null || {
    echo "privacy verifier did not report expected $label leak" >&2
    cat "$work/$label.err" >&2
    exit 1
  }
}

expect_privacy_failure "repository path" "$repo_root"
expect_privacy_failure "home path" "${HOME:-}"
expect_privacy_failure "repository file URL" "file://$repo_root"
expect_privacy_failure "home file URL" "file://${HOME:-}"

rm -rf "$dist" "$payload"
mkdir -p "$dist" "$payload/vectis-$version/bin" "$work/fakebin"
printf 'not really an elf\n' >"$payload/vectis-$version/bin/vectis"
tar -C "$payload" -czf "$dist/vectis-$version.tar.gz" "vectis-$version"
(cd "$dist" && sha256sum "vectis-$version.tar.gz" >"vectis-$version-CHECKSUMS")
cat >"$work/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1" in
  -h) exit 0 ;;
  -d)
    printf '%s\n' ' 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN:/tmp/vectis-local]'
    exit 0
    ;;
esac
exit 1
EOF
chmod +x "$work/fakebin/readelf"
if PATH="$work/fakebin:$PATH" VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
   "$repo_root/scripts/verify_release_privacy.sh" >"$work/rpath.out" 2>"$work/rpath.err"; then
  echo "privacy verifier accepted mixed relocatable and absolute RPATH" >&2
  exit 1
fi
grep -F "non-relocatable ELF runtime path" "$work/rpath.err" >/dev/null || {
  echo "privacy verifier did not report non-relocatable RPATH" >&2
  cat "$work/rpath.err" >&2
  exit 1
}

write_darwin_artifact() {
  rm -rf "$dist" "$payload"
  mkdir -p "$dist" "$payload/vectis-$version-arm64-apple-darwin/lib" "$work/fakebin"
  printf 'not really a mach-o\n' >"$payload/vectis-$version-arm64-apple-darwin/lib/libvectis.0.dylib"
  tar -C "$payload" -czf "$dist/vectis-$version-arm64-apple-darwin.tar.gz" \
    "vectis-$version-arm64-apple-darwin"
  (cd "$dist" && sha256sum "vectis-$version-arm64-apple-darwin.tar.gz" >"vectis-$version-CHECKSUMS")
}

cat >"$work/fakebin/otool" <<'EOF'
#!/bin/sh
mode=${VECTIS_FAKE_OTOOL_CASE:-ok}
option=$1
file=$2

case "$option" in
  -D)
    printf '%s:\n' "$file"
    case "$mode" in
      bad_install_name) printf '%s\n' '/usr/local/lib/libvectis.0.dylib' ;;
      *) printf '%s\n' '@rpath/libvectis.0.dylib' ;;
    esac
    ;;
  -L)
    printf '%s:\n' "$file"
    case "$mode" in
      bad_dependency)
        printf '\t%s\n' '/lib/libbad.dylib (compatibility version 1.0.0, current version 1.0.0)'
        ;;
      *)
        printf '\t%s\n' '/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)'
        ;;
    esac
    ;;
  -l)
    case "$mode" in
      bad_rpath)
        cat <<'RPATH'
Load command 1
          cmd LC_RPATH
      cmdsize 32
         path /tmp/vectis-local (offset 12)
RPATH
        ;;
      *)
        cat <<'LOADS'
Load command 1
          cmd LC_CODE_SIGNATURE
      cmdsize 16
LOADS
        ;;
    esac
    ;;
  *) exit 1 ;;
esac
EOF
chmod +x "$work/fakebin/otool"

expect_darwin_failure() {
  label=$1
  mode=$2

  write_darwin_artifact
  if VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist VECTIS_OTOOL="$work/fakebin/otool" \
     VECTIS_FAKE_OTOOL_CASE=$mode \
     "$repo_root/scripts/verify_release_privacy.sh" >"$work/$mode.out" 2>"$work/$mode.err"; then
    echo "privacy verifier accepted $label" >&2
    exit 1
  fi
  grep -F "$label" "$work/$mode.err" >/dev/null || {
    echo "privacy verifier did not report expected $label" >&2
    cat "$work/$mode.err" >&2
    exit 1
  }
}

expect_darwin_failure "Darwin project dylib install name is not @rpath-relative" bad_install_name
expect_darwin_failure "non-system absolute Darwin dependency path" bad_dependency
expect_darwin_failure "non-relocatable Darwin rpath" bad_rpath

rm -rf "$dist" "$payload"
mkdir -p "$dist" "$payload/vectis-$version-arm64-apple-darwin/lib" "$work/fakebin"
printf 'not really a static archive\n' >"$payload/vectis-$version-arm64-apple-darwin/lib/liblua.a"
tar -C "$payload" -czf "$dist/vectis-$version-arm64-apple-darwin.tar.gz" \
  "vectis-$version-arm64-apple-darwin"
(cd "$dist" && sha256sum "vectis-$version-arm64-apple-darwin.tar.gz" >"vectis-$version-CHECKSUMS")
cat >"$work/fakebin/otool" <<EOF
#!/bin/sh
printf '%s:\\n' "\$2"
printf '%s\\n' '$repo_root/build/not-a-load-command/liblua.a(lapi.o):'
EOF
chmod +x "$work/fakebin/otool"
VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist VECTIS_OTOOL="$work/fakebin/otool" \
  "$repo_root/scripts/verify_release_privacy.sh" >"$work/static-archive.out" 2>"$work/static-archive.err" || {
    echo "privacy verifier rejected Darwin static archive member label" >&2
    cat "$work/static-archive.err" >&2
    exit 1
  }

echo "release privacy contracts ok"
