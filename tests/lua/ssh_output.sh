#!/usr/bin/env bash
set -euo pipefail
binary=${1:?Vectis executable required}
python=${2:?Python fixture interpreter required}
root=${3:?Owned test root required}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root"
work=$(mktemp -d "$root/ssh-wire.XXXXXXXX")
fixture=
cleanup() {
  if [[ -n $fixture ]]; then
    kill "$fixture" 2>/dev/null || true
    wait "$fixture" 2>/dev/null || true
  fi
  rm -rf -- "$work"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkfifo "$work/ready"
"$python" "$source_dir/ssh_fixture.py" "$work/ready" >"$work/stdout" 2>"$work/stderr" &
fixture=$!
# Readiness is an actual listener/key event, with no startup sleep.
read -r port fingerprint <"$work/ready"
for mode in stderr mixed split-output stdin-eof empty idle eof-before-status \
    completion-timeout sftp-ok sftp-close-failure scp-close-failure; do
  if ! "$binary" "$source_dir/lua/ssh_output.lua" "$port" "$fingerprint" "$mode" "$source_dir/lua/ssh_output.lua"; then
    cat "$work/stderr" >&2
    exit 1
  fi
done
