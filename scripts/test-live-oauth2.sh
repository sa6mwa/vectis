#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ "${VECTIS_LIVE_OAUTH2_ENABLE:-0}" != "1" ]; then
  printf '%s\n' \
    'SKIP: set VECTIS_LIVE_OAUTH2_ENABLE=1 to run live OAuth2/OIDC provider checks'
  exit 0
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

if ! command -v base64 >/dev/null 2>&1; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=base64-missing
next=install a base64 command or provide a standard coreutils-compatible PATH
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

require_env() {
  name=$1
  if [ -z "${!name:-}" ]; then
    cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=configuration
status=failed
class=e2e-service
reason=missing-required-environment
artifact=$name
next=set $name for the live OAuth2 provider before rerunning make prerelease-live
PKT_DIAGNOSTIC_END
EOF
    exit 2
  fi
}

require_env VECTIS_LIVE_OAUTH2_TOKEN_ENDPOINT
require_env VECTIS_LIVE_OAUTH2_CLIENT_ID
require_env VECTIS_LIVE_OAUTH2_CLIENT_SECRET

work=$(mktemp -d "${TMPDIR:-/tmp}/vectis-live-oauth2.XXXXXX")
cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

store="$work/credentials.json"
flow_id=${VECTIS_LIVE_OAUTH2_FLOW_ID:-live-m2m}
subject=${VECTIS_LIVE_OAUTH2_SUBJECT:-live-oauth2}

run_capture() {
  label=$1
  shift
  if ! "$@" >"$work/$label.out" 2>"$work/$label.err"; then
    cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=$label
status=failed
class=e2e-service
reason=live-oauth2-command-failed
artifact=redacted-command-output
next=verify provider URL, client credentials, scope, audience, and vectis auth support
PKT_DIAGNOSTIC_END
EOF
    exit 1
  fi
}

assert_output_contains() {
  label=$1
  pattern=$2
  if ! grep -Eq "$pattern" "$work/$label.out"; then
    cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=$label
status=failed
class=test
reason=missing-expected-output
artifact=$pattern
next=inspect the live provider response and vectis OAuth2 CLI contract
PKT_DIAGNOSTIC_END
EOF
    exit 1
  fi
}

assert_output_line() {
  label=$1
  line=$2
  if ! grep -Fxq "$line" "$work/$label.out"; then
    cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=$label
status=failed
class=test
reason=missing-expected-output-line
artifact=$line
next=inspect the live provider response and vectis OAuth2 CLI contract
PKT_DIAGNOSTIC_END
EOF
    exit 1
  fi
}

assert_output_fixed() {
  label=$1
  text=$2
  if ! grep -Fq "$text" "$work/$label.out"; then
    cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=$label
status=failed
class=test
reason=missing-expected-output-text
artifact=$text
next=inspect the live provider response and vectis OAuth2 CLI contract
PKT_DIAGNOSTIC_END
EOF
    exit 1
  fi
}

oauth_args=(
  "$vectis_bin" -a oauth2
  --store "$store"
  --client-credentials "$flow_id"
  --subject "$subject"
  --token-endpoint "$VECTIS_LIVE_OAUTH2_TOKEN_ENDPOINT"
  --client-id "$VECTIS_LIVE_OAUTH2_CLIENT_ID"
  --client-secret "$VECTIS_LIVE_OAUTH2_CLIENT_SECRET"
)
if [ -n "${VECTIS_LIVE_OAUTH2_SCOPE:-}" ]; then
  oauth_args+=(--scope "$VECTIS_LIVE_OAUTH2_SCOPE")
fi
if [ -n "${VECTIS_LIVE_OAUTH2_AUDIENCE:-}" ]; then
  oauth_args+=(--audience "$VECTIS_LIVE_OAUTH2_AUDIENCE")
fi
if [ -n "${VECTIS_LIVE_OAUTH2_RESOURCE:-}" ]; then
  oauth_args+=(--resource "$VECTIS_LIVE_OAUTH2_RESOURCE")
fi
if [ -n "${VECTIS_LIVE_OAUTH2_WEBDAV_CLIENT_ID:-}" ]; then
  oauth_args+=(--webdav-client-id "$VECTIS_LIVE_OAUTH2_WEBDAV_CLIENT_ID")
fi
if [ -n "${VECTIS_LIVE_OAUTH2_MAX_RESPONSE_BYTES:-}" ]; then
  oauth_args+=(--max-response-bytes "$VECTIS_LIVE_OAUTH2_MAX_RESPONSE_BYTES")
fi
if [ -n "${VECTIS_LIVE_OAUTH2_MAX_BODY_BYTES:-}" ]; then
  oauth_args+=(--max-body-bytes "$VECTIS_LIVE_OAUTH2_MAX_BODY_BYTES")
fi

run_capture oauth2_client_credentials "${oauth_args[@]}"
assert_output_line oauth2_client_credentials "stored_flow=$flow_id"
assert_output_contains oauth2_client_credentials '^access_token='

run_capture oauth2_load_flow \
  "$vectis_bin" -a oauth2 --store "$store" --load-flow "$flow_id"
assert_output_contains oauth2_load_flow '^found=true$'
assert_output_line oauth2_load_flow "flow_id=$flow_id"
assert_output_line oauth2_load_flow "subject=$subject"

run_capture oauth2_webdav_key \
  "$vectis_bin" -a oauth2 --store "$store" --webdav-key "$flow_id" \
  --subject "$subject"
assert_output_contains oauth2_webdav_key '^client_id='
assert_output_contains oauth2_webdav_key '^client_secret='
assert_output_fixed oauth2_webdav_key "\"oauth2_flow_id\":\"$flow_id\""

webdav_client_id=$(sed -n 's/^client_id=//p' "$work/oauth2_webdav_key.out" | tail -n 1)
webdav_client_secret=$(
  sed -n 's/^client_secret=//p' "$work/oauth2_webdav_key.out" | tail -n 1
)
if [ -z "$webdav_client_id" ] || [ -z "$webdav_client_secret" ]; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=oauth2-webdav-key
status=failed
class=test
reason=missing-webdav-basic-secret
next=inspect vectis OAuth2 WebDAV-key output contract
PKT_DIAGNOSTIC_END
EOF
  exit 1
fi

basic_secret=$(printf '%s' "$webdav_client_id:$webdav_client_secret" |
  base64 | tr -d '\n')
run_capture webdav_basic_verify \
  "$vectis_bin" -a credentials --store "$store" --verify \
  "Basic $basic_secret" --basic
assert_output_contains webdav_basic_verify '^authenticated=true$'
assert_output_contains webdav_basic_verify '^auth_mode=basic$'
assert_output_fixed webdav_basic_verify "\"oauth2_flow_id\":\"$flow_id\""

if [ -n "${VECTIS_LIVE_OIDC_AUTHORIZATION_ENDPOINT:-}" ]; then
  redirect_uri=${VECTIS_LIVE_OIDC_REDIRECT_URI:-http://127.0.0.1:8787/auth/callback}
  authorize_args=(
    "$vectis_bin" -a oauth2
    --authorize
    --authorization-endpoint "$VECTIS_LIVE_OIDC_AUTHORIZATION_ENDPOINT"
    --client-id "$VECTIS_LIVE_OAUTH2_CLIENT_ID"
    --redirect-uri "$redirect_uri"
    --state vectis-live-state
    --nonce vectis-live-nonce
  )
  if [ -n "${VECTIS_LIVE_OAUTH2_SCOPE:-}" ]; then
    authorize_args+=(--scope "$VECTIS_LIVE_OAUTH2_SCOPE")
  fi
  if [ -n "${VECTIS_LIVE_OAUTH2_AUDIENCE:-}" ]; then
    authorize_args+=(--audience "$VECTIS_LIVE_OAUTH2_AUDIENCE")
  fi
  if [ -n "${VECTIS_LIVE_OAUTH2_RESOURCE:-}" ]; then
    authorize_args+=(--resource "$VECTIS_LIVE_OAUTH2_RESOURCE")
  fi
  run_capture oidc_authorize "${authorize_args[@]}"
  assert_output_contains oidc_authorize '^authorization_url='
  assert_output_contains oidc_authorize '^state=vectis-live-state$'
  assert_output_contains oidc_authorize '^nonce=vectis-live-nonce$'
  oidc_authorize_status=ok
else
  oidc_authorize_status=skipped
fi

printf 'live_oauth2=ok\n'
printf 'flow_id=%s\n' "$flow_id"
printf 'subject=%s\n' "$subject"
printf 'oidc_authorize=%s\n' "$oidc_authorize_status"
