#!/usr/bin/env bash
set -euo pipefail
umask 077
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
binary=${1:?usage: test_postgres_xa.sh VECTIS}
test_script=${VECTIS_POSTGRES_TEST_SCRIPT:-"$repo_root/tests/lua/postgres_xa.lua"}
command -v podman >/dev/null
[[ $(podman info --format '{{.Host.Security.Rootless}}') == true ]] || {
  echo 'PostgreSQL XA fixture requires rootless Podman' >&2; exit 2;
}
mkdir -p "$repo_root/build/postgres-xa"
work=$(mktemp -d "$repo_root/build/postgres-xa/run.XXXXXXXX")
name="vectis-pg-$(basename "$work" | tr '[:upper:].' '[:lower:]-')"
port=${VECTIS_TEST_POSTGRES_PORT:-$(printf '%s' "$work" | cksum | awk '{print 53000 + $1 % 10000}')}
mkdir "$work/data"
cleanup() {
  local status=$?
  trap - EXIT
  if (( status )); then podman logs "$name-postgres" >&2 || true; fi
  if ! podman kube down "$work/postgres.yaml" >"$work/down.log" 2>&1; then
    cat "$work/down.log" >&2
    exit 1
  fi
  if (( status )); then printf 'PostgreSQL XA evidence: %s\n' "$work" >&2
  else rm -rf -- "$work"; fi
  exit "$status"
}
cmake -DNAME="$name" -DPORT="$port" -DSTATE="$work/data" -DDEST="$work" \
  -P "$repo_root/tests/services/render_postgres.cmake"
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
podman kube play --userns=keep-id:uid=70,gid=70 "$work/postgres.yaml" >"$work/up.log" 2>&1
# Actual protocol health, not a startup delay or fixed sleep.
timeout 35 podman wait --condition=healthy "$name-postgres" >"$work/ready.log"
if [[ -n ${VECTIS_POSTGRES_NATIVE_TEST:-} ]]; then
  VECTIS_PG_PORT="$port" VECTIS_PG_PASSWORD_FILE="$work/password" "$VECTIS_POSTGRES_NATIVE_TEST"
fi
if [[ -n ${VECTIS_POSTGRES_OOM_TEST:-} ]]; then
  VECTIS_PG_PORT="$port" VECTIS_PG_PASSWORD_FILE="$work/password" "$VECTIS_POSTGRES_OOM_TEST" "$repo_root/tests/lua/pg_connection_oom.lua"
fi
VECTIS_PG_PORT="$port" VECTIS_PG_PASSWORD_FILE="$work/password" \
  VECTIS_PG_GC_SCRIPT="$repo_root/tests/lua/postgres_gc.lua" \
  VECTIS_UPGRADE_TEST_ROOT="$work/store" "$binary" "$test_script"
"$binary" -a pack --script "$test_script" --output "$work/packed"
VECTIS_PG_PORT="$port" VECTIS_PG_PASSWORD_FILE="$work/password" \
  VECTIS_PG_GC_SCRIPT="$repo_root/tests/lua/postgres_gc.lua" \
  VECTIS_UPGRADE_TEST_ROOT="$work/packed-store" "$work/packed"
