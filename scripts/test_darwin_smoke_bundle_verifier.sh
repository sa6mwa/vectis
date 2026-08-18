#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
verifier="$repo_root/scripts/verify_darwin_smoke_bundle.sh"
work=${TMPDIR:-/tmp}/vectis-darwin-smoke-bundle-test.$$

cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

mkdir -p "$work/tools" "$work/bundle/vectis-smoke/bin" \
  "$work/bundle/vectis-smoke/lib"

cat >"$work/tools/file" <<'EOF'
#!/usr/bin/env bash
set -eu
printf '%s: Mach-O 64-bit executable arm64\n' "$1"
EOF

cat >"$work/tools/codesign" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'codesign:%s\n' "$*" >>"${VECTIS_FAKE_TOOL_LOG:?}"
if [ "${VECTIS_FAKE_CODESIGN_MUTATE:-0}" = "1" ]; then
  printf 'mutated-by-codesign\n' >>"${@: -1}"
fi
if [ "${VECTIS_FAKE_CODESIGN_FAIL:-0}" = "1" ]; then
  exit 77
fi
EOF

cat >"$work/tools/spctl" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'spctl:%s\n' "$*" >>"${VECTIS_FAKE_TOOL_LOG:?}"
if [ "${VECTIS_FAKE_SPCTL_MUTATE:-0}" = "1" ]; then
  printf 'mutated-by-spctl\n' >>"${@: -1}"
fi
if [ "${VECTIS_FAKE_SPCTL_FAIL:-0}" = "1" ]; then
  exit 78
fi
EOF

chmod +x "$work/tools/file" "$work/tools/codesign" "$work/tools/spctl"

cat >"$work/bundle/vectis-smoke/run-smoke.sh" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'vectis darwin smoke ok\n'
EOF
printf 'fake mach-o executable\n' >"$work/bundle/vectis-smoke/bin/vectis_static_smoke"
printf 'fake mach-o dylib\n' >"$work/bundle/vectis-smoke/lib/libvectis.dylib"
chmod +x "$work/bundle/vectis-smoke/run-smoke.sh" \
  "$work/bundle/vectis-smoke/bin/vectis_static_smoke"

(cd "$work/bundle" && "${CMAKE:-cmake}" -E tar cf "$work/vectis-smoke.zip" \
  --format=zip \
  vectis-smoke/run-smoke.sh \
  vectis-smoke/bin/vectis_static_smoke \
  vectis-smoke/lib/libvectis.dylib)

export VECTIS_FILE="$work/tools/file"
export VECTIS_CODESIGN="$work/tools/codesign"
export VECTIS_SPCTL="$work/tools/spctl"
export VECTIS_FAKE_TOOL_LOG="$work/tools.log"

"$verifier" --zip "$work/vectis-smoke.zip" --require-spctl >"$work/ok.out"
grep -F "darwin smoke bundle verification ok: $work/vectis-smoke.zip" \
  "$work/ok.out" >/dev/null
grep -F 'codesign:--verify --strict --verbose=4' "$work/tools.log" \
  >/dev/null
grep -F 'spctl:--assess --type execute' "$work/tools.log" >/dev/null

if VECTIS_FAKE_CODESIGN_MUTATE=1 "$verifier" --zip "$work/vectis-smoke.zip" \
   >"$work/codesign-mutate.out" 2>"$work/codesign-mutate.err"; then
  echo "Darwin smoke bundle verifier accepted codesign mutation" >&2
  exit 1
fi
grep -F 'binary changed during codesign verification' \
  "$work/codesign-mutate.err" >/dev/null

if VECTIS_FAKE_SPCTL_MUTATE=1 "$verifier" --zip "$work/vectis-smoke.zip" \
   >"$work/spctl-mutate.out" 2>"$work/spctl-mutate.err"; then
  echo "Darwin smoke bundle verifier accepted spctl mutation" >&2
  exit 1
fi
grep -F 'binary changed during spctl assessment' \
  "$work/spctl-mutate.err" >/dev/null

if VECTIS_SPCTL="$work/tools/missing-spctl" "$verifier" \
   --zip "$work/vectis-smoke.zip" --require-spctl \
   >"$work/missing-spctl.out" 2>"$work/missing-spctl.err"; then
  echo "Darwin smoke bundle verifier accepted missing required spctl" >&2
  exit 1
fi
grep -F 'spctl unavailable' "$work/missing-spctl.err" >/dev/null

mkdir -p "$work/bad-root/one" "$work/bad-root/two"
(cd "$work/bad-root" && "${CMAKE:-cmake}" -E tar cf "$work/bad-root.zip" \
  --format=zip one two)
if "$verifier" --zip "$work/bad-root.zip" \
   >"$work/bad-root.out" 2>"$work/bad-root.err"; then
  echo "Darwin smoke bundle verifier accepted multiple roots" >&2
  exit 1
fi
grep -F 'zip must contain exactly one root directory' \
  "$work/bad-root.err" >/dev/null

echo "darwin smoke bundle verifier ok"
