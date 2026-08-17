#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ "${VECTIS_LUA_CAI_LIVE:-0}" != "1" ]; then
  printf '%s\n' \
    'SKIP: set VECTIS_LUA_CAI_LIVE=1 and provider credentials to run live CAI provider validation'
  exit 0
fi

provider=${VECTIS_LUA_CAI_PROVIDER:-openai}
api_key_env=${VECTIS_LUA_CAI_API_KEY_ENV:-}
if [ -z "$api_key_env" ]; then
  if [ "$provider" = "openrouter" ]; then
    api_key_env=OPENROUTER_API_KEY
  else
    api_key_env=OPENAI_API_KEY
  fi
fi

if [ -z "${VECTIS_LUA_CAI_API_KEY:-}" ] && [ -z "${!api_key_env:-}" ]; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=tool-discovery
status=failed
class=missing-credential
reason=cai-live-credentials-missing
provider=$provider
api_key_env=$api_key_env
next=set VECTIS_LUA_CAI_API_KEY or $api_key_env, or unset VECTIS_LUA_CAI_LIVE to skip
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

vectis_bin=${VECTIS_BIN:-"$repo_root/build/debug/vectis"}
if [ ! -x "$vectis_bin" ]; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=vectis-binary-missing
artifact=$vectis_bin
next=run make build-debug or set VECTIS_BIN to a built vectis executable
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

VECTIS_LUA_CAI_LIVE=1 "$vectis_bin" \
  "$repo_root/examples/lua/cai_live_service_adapter.lua"
