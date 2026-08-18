#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname -- "$0")/.." && pwd)
script="$repo_root/scripts/verify_darwin_pack_signature.sh"
work=${TMPDIR:-/tmp}/vectis-darwin-pack-signature-test.$$
log="$work/tool.log"

cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT

mkdir -p "$work"
binary="$work/vectis-packed"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$binary"
chmod +x "$binary"

codesign_tool="$work/codesign"
spctl_tool="$work/spctl"

cat >"$codesign_tool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'codesign:%s\n' "$*" >>"${VECTIS_FAKE_TOOL_LOG:?}"
if [ "${VECTIS_FAKE_CODESIGN_MUTATE:-0}" = "1" ]; then
  printf 'mutated-by-codesign\n' >>"${*: -1}"
fi
if [ "${VECTIS_FAKE_CODESIGN_FAIL:-0}" = "1" ]; then
  exit 1
fi
exit 0
EOF
chmod +x "$codesign_tool"

cat >"$spctl_tool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'spctl:%s\n' "$*" >>"${VECTIS_FAKE_TOOL_LOG:?}"
if [ "${VECTIS_FAKE_SPCTL_MUTATE:-0}" = "1" ]; then
  printf 'mutated-by-spctl\n' >>"${*: -1}"
fi
if [ "${VECTIS_FAKE_SPCTL_FAIL:-0}" = "1" ]; then
  exit 1
fi
exit 0
EOF
chmod +x "$spctl_tool"

VECTIS_FAKE_TOOL_LOG="$log" "$script" --binary "$binary" \
  --codesign-tool "$codesign_tool" --spctl-tool "$spctl_tool" >"$work/ok.out"
grep -F "codesign:--verify --strict --verbose=4 $binary" "$log" >/dev/null
grep -F "spctl:--assess --type execute $binary" "$log" >/dev/null
grep -F "darwin pack signature verification ok: $binary" "$work/ok.out" \
  >/dev/null

rm -f "$log"
VECTIS_FAKE_TOOL_LOG="$log" "$script" --binary "$binary" \
  --codesign-tool "$codesign_tool" \
  --spctl-tool "$work/missing-spctl" >"$work/no-spctl.out" \
  2>"$work/no-spctl.err"
grep -F "codesign:--verify --strict --verbose=4 $binary" "$log" >/dev/null
grep -F "skipping spctl, tool unavailable" "$work/no-spctl.err" >/dev/null

if VECTIS_FAKE_TOOL_LOG="$log" "$script" --binary "$binary" \
  --codesign-tool "$codesign_tool" \
  --spctl-tool "$work/missing-spctl" --require-spctl \
  >"$work/require-spctl.out" 2>"$work/require-spctl.err"; then
  echo "Darwin signature verifier accepted missing required spctl" >&2
  exit 1
fi
grep -F "spctl unavailable" "$work/require-spctl.err" >/dev/null

if VECTIS_FAKE_CODESIGN_FAIL=1 VECTIS_FAKE_TOOL_LOG="$log" "$script" \
  --binary "$binary" --codesign-tool "$codesign_tool" \
  --spctl-tool "$spctl_tool" >"$work/codesign-fail.out" \
  2>"$work/codesign-fail.err"; then
  echo "Darwin signature verifier accepted codesign failure" >&2
  exit 1
fi

printf '%s\n' '#!/bin/sh' 'exit 0' >"$binary"
chmod +x "$binary"
if VECTIS_FAKE_CODESIGN_MUTATE=1 VECTIS_FAKE_TOOL_LOG="$log" "$script" \
  --binary "$binary" --codesign-tool "$codesign_tool" \
  --spctl-tool "$spctl_tool" >"$work/codesign-mutate.out" \
  2>"$work/codesign-mutate.err"; then
  echo "Darwin signature verifier accepted codesign verification mutation" >&2
  exit 1
fi
grep -F "binary changed during codesign verification" \
  "$work/codesign-mutate.err" >/dev/null

printf '%s\n' '#!/bin/sh' 'exit 0' >"$binary"
chmod +x "$binary"
if VECTIS_FAKE_SPCTL_MUTATE=1 VECTIS_FAKE_TOOL_LOG="$log" "$script" \
  --binary "$binary" --codesign-tool "$codesign_tool" \
  --spctl-tool "$spctl_tool" >"$work/spctl-mutate.out" \
  2>"$work/spctl-mutate.err"; then
  echo "Darwin signature verifier accepted spctl assessment mutation" >&2
  exit 1
fi
grep -F "binary changed during spctl assessment" "$work/spctl-mutate.err" \
  >/dev/null

if "$script" --binary "$work/missing-binary" --codesign-tool "$codesign_tool" \
  --spctl-tool "$spctl_tool" >"$work/missing-binary.out" \
  2>"$work/missing-binary.err"; then
  echo "Darwin signature verifier accepted missing binary" >&2
  exit 1
fi
grep -F "binary not found" "$work/missing-binary.err" >/dev/null

nonexec="$work/nonexec"
printf 'not executable\n' >"$nonexec"
if "$script" --binary "$nonexec" --codesign-tool "$codesign_tool" \
  --spctl-tool "$spctl_tool" >"$work/nonexec.out" \
  2>"$work/nonexec.err"; then
  echo "Darwin signature verifier accepted non-executable binary" >&2
  exit 1
fi
grep -F "binary is not executable" "$work/nonexec.err" >/dev/null

echo "darwin pack signature verifier ok"
