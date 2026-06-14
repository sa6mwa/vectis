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

echo "release privacy contracts ok"
