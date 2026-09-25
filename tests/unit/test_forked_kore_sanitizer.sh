#!/usr/bin/env bash
set -euo pipefail

wrapper=$1
test_dir=$(mktemp -d "$PWD/vectis-forked-sanitizer.XXXXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT

cat > "$test_dir/clean" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat > "$test_dir/report" <<'EOF'
#!/usr/bin/env bash
set -eu
report_path=${ASAN_OPTIONS##*log_path=}
report_path=${report_path%%:*}
printf 'ERROR: AddressSanitizer: injected worker failure\n' > "${report_path}.$$"
exit 0
EOF
chmod +x "$test_dir/clean" "$test_dir/report"

(
  cd "$test_dir"
  bash "$wrapper" 5 0 "$test_dir/clean" > clean.output 2>&1
  if bash "$wrapper" 5 0 "$test_dir/report" > report.output 2>&1; then
    printf 'forked Kore wrapper ignored a worker sanitizer report\n' >&2
    exit 1
  fi
  grep -q 'forked Kore worker sanitizer report:' report.output
  grep -q 'AddressSanitizer: injected worker failure' report.output
  if compgen -G 'vectis-kore-test.*' > /dev/null; then
    printf 'forked Kore wrapper left a test directory behind\n' >&2
    exit 1
  fi
)
