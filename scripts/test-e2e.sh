#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

disk_endpoint=${VECTIS_E2E_LOCKD_DISK_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_DISK_PORT:-29441}}
s3_endpoint=${VECTIS_E2E_LOCKD_S3_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_S3_PORT:-29443}}
client_bundle=${VECTIS_E2E_LOCKD_CLIENT_BUNDLE:-$repo_root/devenv/volumes/lockd-config/client.pem}
ssh_port=${VECTIS_SSH_PORT:-29222}
mqtt_port=${VECTIS_MQTT_PORT:-21883}
default_kore_basic_port=$((31080 + ($$ % 1000) * 8))
kore_basic_port=${VECTIS_E2E_KORE_BASIC_PORT:-$default_kore_basic_port}
kore_lockd_port=${VECTIS_E2E_KORE_LOCKD_PORT:-$((kore_basic_port + 1))}
kore_workflow_port=${VECTIS_E2E_KORE_WORKFLOW_PORT:-$((kore_basic_port + 2))}
kore_downstream_port=${VECTIS_E2E_KORE_DOWNSTREAM_PORT:-$((kore_basic_port + 3))}
kore_static_port=${VECTIS_E2E_KORE_STATIC_PORT:-$((kore_basic_port + 4))}
kore_combined_port=${VECTIS_E2E_KORE_COMBINED_PORT:-$((kore_basic_port + 5))}
kore_packed_port=${VECTIS_E2E_KORE_PACKED_PORT:-$((kore_basic_port + 6))}
kore_packed_https_port=${VECTIS_E2E_KORE_PACKED_HTTPS_PORT:-$((kore_basic_port + 7))}
kore_acme_port=${VECTIS_E2E_KORE_ACME_PORT:-$((kore_basic_port + 8))}
lua_consumer_port=${VECTIS_E2E_LUA_CONSUMER_PORT:-$((kore_basic_port + 9))}
https_runtime_port=${VECTIS_E2E_HTTPS_RUNTIME_PORT:-$((kore_basic_port + 10))}
https_mtls_runtime_port=${VECTIS_E2E_HTTPS_MTLS_RUNTIME_PORT:-$((kore_basic_port + 11))}
acme_mock_port=${VECTIS_E2E_ACME_MOCK_PORT:-$((kore_basic_port + 12))}
pack_smtp_harness=${VECTIS_E2E_PACK_SMTP_HARNESS:-$repo_root/build/debug/tests/vectis_pack_smtp_harness}
acme_mock_provider=${VECTIS_E2E_ACME_MOCK_PROVIDER:-$repo_root/build/debug/tests/vectis_acme_mock_provider}
work_dir=$(mktemp -d)
ssh_memory_key="$work_dir/vectis-e2e-ssh-key"
ssh_bad_host_key="$work_dir/vectis-e2e-bad-host-key"
ssh_known_hosts="$work_dir/vectis-e2e-known-hosts"
ssh_bad_known_hosts="$work_dir/vectis-e2e-bad-known-hosts"
ssh_host_key_sha256=""
ssh_bad_host_key_sha256="SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
server_pids=""

cleanup() {
  for pid in $server_pids; do
    kill -- "-$pid" >/dev/null 2>&1 || kill "$pid" >/dev/null 2>&1 || true
  done
  sleep 1
  for pid in $server_pids; do
    kill -9 -- "-$pid" >/dev/null 2>&1 || kill -9 "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  if [ "${VECTIS_E2E_KEEP_WORK:-0}" = "1" ]; then
    printf '%s\n' "vectis e2e work dir preserved at $work_dir" >&2
  else
    rm -rf "$work_dir"
  fi
  if [ "${VECTIS_E2E_KEEP_DEVSERVICES:-0}" != "1" ]; then
    "$script_dir/dev-down.sh" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

wait_for_http() {
  url=$1
  label=$2
  logfile=${3:-}
  count=0
  while ! curl --max-time 3 -fsS "$url" >/dev/null 2>&1; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for $label at $url" >&2
      if [ -n "$logfile" ] && [ -f "$logfile" ]; then
        sed "s/^/[$label] /" "$logfile" >&2
      fi
      return 1
    fi
    sleep 1
  done
}

wait_for_https_insecure() {
  url=$1
  label=$2
  count=0
  while ! curl --max-time 3 -k -fsS "$url" >/dev/null 2>&1; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for $label at $url" >&2
      return 1
    fi
    sleep 1
  done
}

start_server() {
  label=$1
  logfile=$2
  shift 2

  if command -v setsid >/dev/null 2>&1; then
    setsid "$@" >"$logfile" 2>&1 &
  else
    "$@" >"$logfile" 2>&1 &
  fi
  pid=$!
  server_pids="$server_pids $pid"
  printf '[e2e] %s pid=%s\n' "$label" "$pid"
  sleep 1
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    printf '%s\n' "$label exited during startup" >&2
    sed 's/^/[server] /' "$logfile" >&2
    return 1
  fi
}

curl_or_log() {
  logfile=$1
  label=$2
  status=0
  shift 2

  set +e
  curl "$@"
  status=$?
  set -e
  if [ "$status" -ne 0 ]; then
    printf '%s\n' "curl failed for $label" >&2
    if [ -f "$logfile" ]; then
      sed "s/^/[$label] /" "$logfile" >&2
    else
      printf '%s\n' "missing server log for $label at $logfile" >&2
    fi
    return "$status"
  fi
}

curl_body_or_log() {
  logfile=$1
  label=$2
  bodyfile=$3
  headers="${bodyfile}.headers"
  status=0
  shift 3

  set +e
  curl "$@" -D "$headers" -o "$bodyfile"
  status=$?
  set -e
  if [ "$status" -ne 0 ]; then
    printf '%s\n' "curl failed for $label" >&2
    if [ -f "$headers" ]; then
      sed "s/^/[$label.headers] /" "$headers" >&2
    fi
    if [ -f "$bodyfile" ]; then
      sed "s/^/[$label.body] /" "$bodyfile" >&2
    fi
    if [ -f "$logfile" ]; then
      sed "s/^/[$label.server] /" "$logfile" >&2
    else
      printf '%s\n' "missing server log for $label at $logfile" >&2
    fi
    return "$status"
  fi
}

curl_head_or_log() {
  logfile=$1
  label=$2
  headers=$3
  status=0
  shift 3

  set +e
  curl "$@" -D "$headers" -o /dev/null --head
  status=$?
  set -e
  if [ "$status" -ne 0 ]; then
    printf '%s\n' "curl failed for $label" >&2
    if [ -f "$headers" ]; then
      sed "s/^/[$label.headers] /" "$headers" >&2
    fi
    if [ -f "$logfile" ]; then
      sed "s/^/[$label.server] /" "$logfile" >&2
    else
      printf '%s\n' "missing server log for $label at $logfile" >&2
    fi
    return "$status"
  fi
}

assert_no_store_headers() {
  header_file=$1
  label=$2

  grep -qi '^cache-control: no-store' "$header_file" || {
    printf '%s\n' "$label response did not include cache-control: no-store" >&2
    cat "$header_file" >&2
    return 1
  }
  grep -qi '^pragma: no-cache' "$header_file" || {
    printf '%s\n' "$label response did not include pragma: no-cache" >&2
    cat "$header_file" >&2
    return 1
  }
  grep -qi '^expires: 0' "$header_file" || {
    printf '%s\n' "$label response did not include expires: 0" >&2
    cat "$header_file" >&2
    return 1
  }
}

assert_packed_auth_state_record() {
  label=$1
  record_id=$2

  if ! grep -Fq "$record_id" "$packed_service_auth_state"; then
    printf '%s\n' \
      "$label record was not written to packed auth state: $record_id" >&2
    cat "$packed_service_auth_state" >&2
    return 1
  fi
  if grep -Fq "$record_id" "$packed_service_credentials"; then
    printf '%s\n' \
      "$label record leaked into packed credentials: $record_id" >&2
    cat "$packed_service_credentials" >&2
    return 1
  fi
}

assert_packed_logout_requires_authorization() {
  label=$1
  auth_path=$2

  logout_headers="$work_dir/$label-logout-anonymous.headers"
  logout_status=$(curl --max-time 3 -sS -D "$logout_headers" \
    -o "$work_dir/$label-logout-anonymous.txt" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/x-www-form-urlencoded' --data '' \
    "http://127.0.0.1:$kore_packed_port$auth_path/logout")
  logout_body=$(cat "$work_dir/$label-logout-anonymous.txt")
  if [ "$logout_status" != "401" ]; then
    printf '%s\n' \
      "$label anonymous logout returned unexpected status: $logout_status" >&2
    printf '%s\n' "$logout_body" >&2
    return 1
  fi
  assert_no_store_headers "$logout_headers" "$label anonymous logout"
  grep -qi '^www-authenticate: Basic' "$logout_headers" || {
    printf '%s\n' \
      "$label anonymous logout did not request Basic authorization" >&2
    cat "$logout_headers" >&2
    return 1
  }
  if [ "$logout_body" != "authorization is required" ]; then
    printf '%s\n' \
      "$label anonymous logout returned unexpected body: $logout_body" >&2
    return 1
  fi
}

assert_packed_key_webdav_logout() {
  label=$1
  auth_path=$2
  client_id=$3
  client_secret=$4

  body=$(curl_or_log "$packed_service_log" "$label webdav read" \
    --max-time 3 -fsS -u "$client_id:$client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected $label WebDAV body: $body" >&2
      return 1
      ;;
  esac
  logout_headers="$work_dir/$label-logout.headers"
  logout_status=$(curl --max-time 3 -sS -D "$logout_headers" \
    -o "$work_dir/$label-logout.txt" -w '%{http_code}' \
    -u "$client_id:$client_secret" \
    -X POST -H 'Content-Type: application/x-www-form-urlencoded' --data '' \
    "http://127.0.0.1:$kore_packed_port$auth_path/logout")
  logout_body=$(cat "$work_dir/$label-logout.txt")
  if [ "$logout_status" != "200" ]; then
    printf '%s\n' "$label logout returned unexpected status: $logout_status" >&2
    printf '%s\n' "$logout_body" >&2
    return 1
  fi
  assert_no_store_headers "$logout_headers" "$label logout"
  if [ "$logout_body" != "logged_out=1" ]; then
    printf '%s\n' "$label logout returned unexpected body: $logout_body" >&2
    return 1
  fi
  logged_out_api_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$client_id:$client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$logged_out_api_status" != "401" ]; then
    printf '%s\n' "$label logout left guarded API credential active: $logged_out_api_status" >&2
    return 1
  fi
  logged_out_dav_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$client_id:$client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$logged_out_dav_status" != "401" ]; then
    printf '%s\n' "$label logout left WebDAV credential active: $logged_out_dav_status" >&2
    return 1
  fi
}

run_lockd_examples() {
  endpoint=$1
  label=$2

  printf '[e2e] lockd %s open\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_open"

  printf '[e2e] lockd %s lease save/load/release\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_lease"

  printf '[e2e] lockd %s enqueue/dequeue\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_enqueue"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_dequeue"
}

run_lockd_failure_examples() {
  bad_bundle="$work_dir/bad-client.pem"

  printf '[e2e] lockd unavailable endpoint failure\n'
  if env LOCKD_ENDPOINT="https://127.0.0.1:1" LOCKD_CLIENT_BUNDLE="$client_bundle" \
      "$repo_root/build/debug/examples/vectis_example_lockd_lease"; then
    printf '%s\n' "lockd lease unexpectedly succeeded against unavailable endpoint" >&2
    return 1
  fi

  printf '%s\n' "not a pem bundle" >"$bad_bundle"
  printf '[e2e] lockd invalid client bundle failure\n'
  if env LOCKD_ENDPOINT="$disk_endpoint" LOCKD_CLIENT_BUNDLE="$bad_bundle" \
      "$repo_root/build/debug/examples/vectis_example_lockd_lease"; then
    printf '%s\n' "lockd lease unexpectedly succeeded with invalid client bundle" >&2
    return 1
  fi
}

run_lockd_lua_examples() {
  endpoint=$1
  label=$2

  printf '[e2e] lockd lua %s open/close\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    "$repo_root/build/debug/vectis" "$repo_root/tests/lua/lockdc_open_close.lua"

  printf '[e2e] lockd lua %s state\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    LOCKD_STATE_KEY="lua/e2e/$label/state-$$" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/lockd_state.lua"

  printf '[e2e] lockd lua %s queue\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    LOCKD_QUEUE="lua-e2e-$label-$$" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/lockd_queue.lua"
}

run_lockd_lua_consumer_example() {
  endpoint=$1
  label=$2

  printf '[e2e] lockd lua %s consumer service\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    LOCKD_QUEUE="lua-e2e-consumer-$label-$$" \
    VECTIS_LUA_CONSUMER_EXAMPLE_PORT="$lua_consumer_port" \
    VECTIS_LUA_CONSUMER_EXAMPLE_CACHE="$work_dir/lua-consumer-cache-$label" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/consumer_service.lua"
}

run_service_examples() {
  printf '[e2e] mqtt publish\n'
  env VECTIS_MQTT_URL="mqtt://127.0.0.1:$mqtt_port" \
    "$repo_root/build/debug/examples/vectis_example_mqtt"

  printf '[e2e] ssh command\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    VECTIS_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    VECTIS_SSH_HOST_KEY_SHA256="$ssh_host_key_sha256" \
    "$repo_root/build/debug/examples/vectis_example_ssh"

  printf '[e2e] ssh command accepts SHA-256 host-key fingerprint pin\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    VECTIS_SSH_HOST_KEY_SHA256="$ssh_host_key_sha256" \
    "$repo_root/build/debug/examples/vectis_example_ssh"

  printf '[e2e] ssh command rejects wrong SHA-256 host-key fingerprint pin\n'
  if env VECTIS_SSH_HOST="127.0.0.1" \
      VECTIS_SSH_PORT="$ssh_port" \
      VECTIS_SSH_USERNAME="vectis" \
      VECTIS_SSH_PASSWORD="vectispass" \
      VECTIS_SSH_HOST_KEY_SHA256="$ssh_bad_host_key_sha256" \
      "$repo_root/build/debug/examples/vectis_example_ssh"; then
    printf '%s\n' \
      "libssh2 SSH unexpectedly accepted mismatched host-key fingerprint" >&2
    return 1
  fi

  printf '[e2e] ssh command rejects wrong known_hosts pin\n'
  if env VECTIS_SSH_HOST="127.0.0.1" \
      VECTIS_SSH_PORT="$ssh_port" \
      VECTIS_SSH_USERNAME="vectis" \
      VECTIS_SSH_PASSWORD="vectispass" \
      VECTIS_SSH_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      "$repo_root/build/debug/examples/vectis_example_ssh"; then
    printf '%s\n' "libssh2 SSH unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi

  printf '[e2e] ssh command with memory private key\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PRIVATE_KEY_MEMORY_FILE="$ssh_memory_key" \
    VECTIS_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    "$repo_root/build/debug/examples/vectis_example_ssh"

  printf '[e2e] lua ssh command\n'
  env VECTIS_LUA_SSH_HOST="127.0.0.1" \
    VECTIS_LUA_SSH_PORT="$ssh_port" \
    VECTIS_LUA_SSH_USERNAME="vectis" \
    VECTIS_LUA_SSH_PASSWORD="vectispass" \
    VECTIS_LUA_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    VECTIS_LUA_SSH_HOST_KEY_SHA256="$ssh_host_key_sha256" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/ssh_command.lua"

  lua_ssh_pack="$work_dir/vectis-lua-ssh-command-pack"
  "$repo_root/build/debug/vectis" -a pack \
    --script "$repo_root/examples/lua/ssh_command.lua" \
    --output "$lua_ssh_pack"

  printf '[e2e] packed lua ssh command\n'
  env VECTIS_LUA_SSH_HOST="127.0.0.1" \
    VECTIS_LUA_SSH_PORT="$ssh_port" \
    VECTIS_LUA_SSH_USERNAME="vectis" \
    VECTIS_LUA_SSH_PASSWORD="vectispass" \
    VECTIS_LUA_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    VECTIS_LUA_SSH_HOST_KEY_SHA256="$ssh_host_key_sha256" \
    "$lua_ssh_pack"

  printf '[e2e] lua ssh command rejects wrong SHA-256 host-key fingerprint pin\n'
  if env VECTIS_LUA_SSH_HOST="127.0.0.1" \
      VECTIS_LUA_SSH_PORT="$ssh_port" \
      VECTIS_LUA_SSH_USERNAME="vectis" \
      VECTIS_LUA_SSH_PASSWORD="vectispass" \
      VECTIS_LUA_SSH_HOST_KEY_SHA256="$ssh_bad_host_key_sha256" \
      "$repo_root/build/debug/vectis" "$repo_root/examples/lua/ssh_command.lua"; then
    printf '%s\n' \
      "Lua libssh2 SSH unexpectedly accepted mismatched host-key fingerprint" >&2
    return 1
  fi

  printf '[e2e] lua ssh command rejects wrong known_hosts pin\n'
  if env VECTIS_LUA_SSH_HOST="127.0.0.1" \
      VECTIS_LUA_SSH_PORT="$ssh_port" \
      VECTIS_LUA_SSH_USERNAME="vectis" \
      VECTIS_LUA_SSH_PASSWORD="vectispass" \
      VECTIS_LUA_SSH_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      "$repo_root/build/debug/vectis" "$repo_root/examples/lua/ssh_command.lua"; then
    printf '%s\n' "Lua libssh2 SSH unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi

  printf '[e2e] packed lua ssh command rejects wrong known_hosts pin\n'
  if env VECTIS_LUA_SSH_HOST="127.0.0.1" \
      VECTIS_LUA_SSH_PORT="$ssh_port" \
      VECTIS_LUA_SSH_USERNAME="vectis" \
      VECTIS_LUA_SSH_PASSWORD="vectispass" \
      VECTIS_LUA_SSH_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      "$lua_ssh_pack"; then
    printf '%s\n' \
      "Packed Lua libssh2 SSH unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi

  printf '[e2e] libssh2 sftp upload/download\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    VECTIS_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    VECTIS_SSH_HOST_KEY_SHA256="$ssh_host_key_sha256" \
    "$repo_root/build/debug/examples/vectis_example_ssh_sftp"

  printf '[e2e] libssh2 sftp rejects wrong known_hosts pin\n'
  if env VECTIS_SSH_HOST="127.0.0.1" \
      VECTIS_SSH_PORT="$ssh_port" \
      VECTIS_SSH_USERNAME="vectis" \
      VECTIS_SSH_PASSWORD="vectispass" \
      VECTIS_SSH_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      "$repo_root/build/debug/examples/vectis_example_ssh_sftp"; then
    printf '%s\n' "libssh2 SFTP unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi

  printf '[e2e] curl sftp upload/download\n'
  env VECTIS_SFTP_URL="sftp://127.0.0.1:$ssh_port" \
    VECTIS_SFTP_USERNAME="vectis" \
    VECTIS_SFTP_PASSWORD="vectispass" \
    "$repo_root/build/debug/examples/vectis_example_sftp"

  printf '[e2e] lua sftp upload/download\n'
  env VECTIS_LUA_SFTP_URL="sftp://127.0.0.1:$ssh_port" \
    VECTIS_LUA_SFTP_USERNAME="vectis" \
    VECTIS_LUA_SFTP_PASSWORD="vectispass" \
    VECTIS_LUA_SFTP_KNOWN_HOSTS="$ssh_known_hosts" \
    VECTIS_LUA_SFTP_UPLOAD_FILE="$work_dir/lua-sftp-upload.txt" \
    VECTIS_LUA_SFTP_DOWNLOAD_FILE="$work_dir/lua-sftp-download.txt" \
    VECTIS_LUA_SFTP_REMOTE_FILE="/config/lua-sftp-upload-$$.txt" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/sftp_transfer.lua"

  printf '[e2e] lua stateful sftp handles\n'
  env VECTIS_LUA_SFTP_HOST="127.0.0.1" \
    VECTIS_LUA_SFTP_PORT="$ssh_port" \
    VECTIS_LUA_SFTP_USERNAME="vectis" \
    VECTIS_LUA_SFTP_PASSWORD="vectispass" \
    VECTIS_LUA_SFTP_KNOWN_HOSTS="$ssh_known_hosts" \
    VECTIS_LUA_SFTP_HOST_KEY_SHA256="$ssh_host_key_sha256" \
    VECTIS_LUA_SFTP_REMOTE_FILE="/config/lua-sftp-handles-$$.txt" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/sftp_handles.lua"

  printf '[e2e] lua stateful sftp handles reject wrong known_hosts pin\n'
  if env VECTIS_LUA_SFTP_HOST="127.0.0.1" \
      VECTIS_LUA_SFTP_PORT="$ssh_port" \
      VECTIS_LUA_SFTP_USERNAME="vectis" \
      VECTIS_LUA_SFTP_PASSWORD="vectispass" \
      VECTIS_LUA_SFTP_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      VECTIS_LUA_SFTP_REMOTE_FILE="/config/lua-sftp-handles-bad-$$.txt" \
      "$repo_root/build/debug/vectis" "$repo_root/examples/lua/sftp_handles.lua"; then
    printf '%s\n' "Lua stateful SFTP unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi
}

run_downstream_http_examples() {
  printf '[e2e] downstream HTTP client/server\n'
  start_server "downstream http" "$work_dir/downstream-http.log" \
    env VECTIS_KORE_PORT="$kore_downstream_port" \
      "$repo_root/build/debug/examples/vectis_example_curl_downstream_e2e" server
  wait_for_http "http://127.0.0.1:$kore_downstream_port/health" "downstream HTTP server"
  env VECTIS_DOWNSTREAM_BASE_URL="http://127.0.0.1:$kore_downstream_port" \
    "$repo_root/build/debug/examples/vectis_example_curl_downstream_e2e" client
}

run_static_asset_examples() {
  status=
  body=

  printf '[e2e] kore static assets and traversal guard\n'
  start_server "static assets" "$work_dir/static-assets.log" \
    env VECTIS_KORE_PORT="$kore_static_port" \
      VECTIS_STATIC_ROOT="$work_dir/static-assets" \
      VECTIS_STATIC_INDEX="$work_dir/static-assets/index.html" \
      VECTIS_STATIC_APP="$work_dir/static-assets/app.js" \
      "$repo_root/build/debug/examples/vectis_example_kore_static_assets"
  wait_for_http "http://127.0.0.1:$kore_static_port/" "static asset server"
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_static_port/")
  case "$body" in
    *'<title>vectis</title>'*) ;;
    *)
      printf '%s\n' "Unexpected static index response: $body" >&2
      return 1
      ;;
  esac
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_static_port/assets/app.js")
  case "$body" in
    *"console.log('vectis');"*) ;;
    *)
      printf '%s\n' "Unexpected static app response: $body" >&2
      return 1
      ;;
  esac
  status=$(curl --max-time 3 -fsS -o /dev/null -w '%{http_code}' -I \
    "http://127.0.0.1:$kore_static_port/assets/app.js")
  if [ "$status" != "200" ]; then
    printf '%s\n' "Unexpected static HEAD status: $status" >&2
    return 1
  fi
  for traversal_path in \
    "/assets/../secret" \
    "/assets/%2e%2e/secret" \
    "/assets/%2E%2E/secret" \
    "/assets/..%2fsecret"; do
    status=$(curl --path-as-is --max-time 3 -fsS -o /dev/null -w '%{http_code}' \
      "http://127.0.0.1:$kore_static_port$traversal_path" || true)
    if [ "$status" != "400" ] && [ "$status" != "404" ]; then
      printf '%s\n' "Unexpected traversal status for $traversal_path: $status" >&2
      return 1
    fi
  done
}

run_lua_examples() {
  script="$work_dir/vectis-e2e.lua"
  pack_script="$work_dir/vectis-e2e-pack.lua"
  packed_service_script="$work_dir/vectis-e2e-packed-service.lua"
  packed_https_script="$work_dir/vectis-e2e-packed-https.lua"
  acme_state_script="$work_dir/vectis-e2e-acme-state.lua"
  acme_state_cache="$work_dir/vectis-e2e-acme-cache"
  acme_state_credentials="$work_dir/vectis-e2e-acme-credentials.json"
  acme_state_log="$work_dir/lua-acme-state.log"
  acme_mock_log="$work_dir/acme-mock.log"
  packed_service_site="$work_dir/vectis-e2e-packed-site"
  packed_service_cache="$work_dir/vectis-e2e-packed-cache"
  packed_service_docroot="$packed_service_cache/webdav/packed-service-e2e/content"
  packed_service_credentials="$work_dir/vectis-e2e-packed-credentials.json"
  packed_service_auth_state="$work_dir/vectis-e2e-packed-auth-state.json"
  packed_service_mailbox="$work_dir/vectis-e2e-packed-mailbox.txt"
  packed_service_enqueue="$work_dir/vectis-e2e-packed-enqueue.lua"
  packed_service_ready="$work_dir/vectis-e2e-packed-ready"
  packed_https_cert="$work_dir/vectis-e2e-packed-https.pem"
  packed_content_types="$work_dir/vectis-e2e-packed-content-types.json"
  shebang_script="$work_dir/vectis-e2e-shebang.lua"
  packed="$work_dir/vectis-e2e-packed"
  packed_service="$work_dir/vectis-e2e-packed-service"
  packed_https="$work_dir/vectis-e2e-packed-https"
  packed_service_log="$work_dir/lua-packed-webserver.log"
  packed_https_log="$work_dir/lua-packed-https.log"
  packed_service_queue="vectis-e2e-packed-$$"
  packed_service_namespace="examples"
  auth_headers=
  browser_continue_start_response=
  browser_pending_id=
  webdav_key_response=
  webdav_key_headers=
  webdav_client_id=
  webdav_client_secret=
  static_put_status=
  root_static_put_status=
  static_head_body=
  static_head_headers=
  static_head_status=
  unauth_dav_status=
  traversal_status=
  dav_write_body=
  propfind_body=
  propfind_status=
  method_status=
  guarded_status=
  password_only_status=
  password_key_response=
  password_client_id=
  password_client_secret=
  password_totp_body=
  password_totp_status=
  embedded_template_body=
  wrong_token_status=
  missing_totp_body=
  missing_totp_status=
  wrong_totp_status=
  replay_token_status=
  blocked_email_status=
  unauth_dav_put_status=
  email_token_response=
  email_transaction_id=
  email_token=
  no_totp_key_response=
  no_totp_client_id=
  no_totp_client_secret=
  no_totp_email_token_response=
  no_totp_email_transaction_id=
  no_totp_email_token=
  email_only_key_response=
  email_only_client_id=
  email_only_client_secret=
  email_only_token_response=
  email_only_transaction_id=
  email_only_token=
  email_only_missing_user_body=
  email_only_missing_user_status=
  email_only_unknown_body=
  email_only_unknown_status=
  password_email_key_response=
  password_email_client_id=
  password_email_client_secret=
  password_email_start_response=
  password_email_pending_id=
  password_email_token_response=
  password_email_transaction_id=
  password_email_token=
  password_email_wrong_pending_body=
  password_email_wrong_pending_id=
  password_email_wrong_pending_response=
  password_email_wrong_pending_status=
  totp_start_response=
  totp_pending_id=
  totp_key_response=
  totp_client_id=
  totp_client_secret=
  totp_continue_start_response=
  totp_continue_pending_id=
  totp_continue_response=
  totp_continue_client_id=
  totp_continue_client_secret=
  totp_no_enrollment_body=
  totp_no_enrollment_status=
  limited_token_response=
  limited_transaction_id=
  limited_token=
  limited_wrong_one_status=
  limited_wrong_two_status=
  limited_after_budget_status=
  expired_token_response=
  expired_transaction_id=
  expired_token=
  expired_token_status=
  expired_token_replay_status=
  mail_body=
  packed_consumer_body=
  logout_body=
  logout_headers=
  logout_status=
  logged_out_api_status=
  logged_out_dav_status=

  printf '[e2e] lua runner\n'
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'assert(vectis.status_string(vectis.OK) == "ok")' \
    'assert(arg[1] == "first")' \
    'assert(arg[2] == "second")' >"$script"
  "$repo_root/build/debug/vectis" "$script" first second

  printf '[e2e] lua shebang\n'
  printf '#!%s\n%s\n%s\n' \
    "$repo_root/build/debug/vectis" \
    'local vectis = require("vectis")' \
    'assert(vectis.status_string(vectis.OK) == "ok")' >"$shebang_script"
  chmod 755 "$shebang_script"
  "$shebang_script"

  printf '[e2e] lua pack\n'
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'assert(vectis.status_string(vectis.OK) == "ok")' >"$pack_script"
  "$repo_root/build/debug/vectis" -a pack --script "$pack_script" --output "$packed"
  "$packed"

  printf '[e2e] lua packed webserver\n'
  if [ ! -x "$pack_smtp_harness" ]; then
    printf '%s\n' "packed SMTP harness is missing: $pack_smtp_harness" >&2
    return 1
  fi
  mkdir -p "$packed_service_site/assets" "$packed_service_site/templates"
  printf '%s\n' \
    '<!doctype html>' \
    '<html><head><title>Vectis packed e2e</title></head>' \
    '<body><main id="app">packed service asset</main></body></html>' \
    >"$packed_service_site/index.html"
  printf '%s\n' \
    'window.vectisPackedService = true;' \
    >"$packed_service_site/app.js"
  printf '%s\n' \
    'body { color: #123456; }' \
    >"$packed_service_site/app.css"
  printf '%s\n' \
    'VX packed logo' \
    >"$packed_service_site/assets/logo.txt"
  printf '%s\n' \
    '<form id="packed-login" data-realm="{{ realm }}" action="{{ continue_action }}" method="post">' \
    '<h1>{{ login_title }}</h1>' \
    '<input name="username">' \
    '<a href="{{ webdav_key_action }}">webdav key</a>' \
    '</form>' \
    >"$packed_service_site/templates/login.html"
  printf '%s\n' \
    '{"types":[' \
    '{"extension":".html","content_type":"text/html; charset=utf-8"},' \
    '{"extension":".css","content_type":"text/css"},' \
    '{"extension":".js","content_type":"application/javascript"}' \
    ']}' >"$packed_content_types"
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'local port = tonumber(assert(os.getenv("VECTIS_PACKED_SERVICE_PORT")))' \
    'local credentials_path = assert(os.getenv("VECTIS_PACKED_SERVICE_CREDENTIALS"))' \
    'local auth_state_path = assert(os.getenv("VECTIS_PACKED_SERVICE_AUTH_STATE"))' \
    'local cache_dir = assert(os.getenv("VECTIS_PACKED_SERVICE_CACHE"))' \
    'local smtp_url = assert(os.getenv("VECTIS_PACK_SMTP_URL"))' \
    'local ready_path = assert(os.getenv("VECTIS_PACKED_SERVICE_READY"))' \
    'local lockd_endpoint = assert(os.getenv("VECTIS_PACKED_SERVICE_LOCKD_ENDPOINT"))' \
    'local lockd_queue = assert(os.getenv("VECTIS_PACKED_SERVICE_LOCKD_QUEUE"))' \
    'local lockd_namespace = os.getenv("VECTIS_PACKED_SERVICE_LOCKD_NAMESPACE") or "examples"' \
    'assert(vectis.embedded.has_assets())' \
    'local index = assert(vectis.embedded.read("/index.html"))' \
    'assert(index:match("packed service asset"))' \
    'local root_listing = table.concat(assert(vectis.embedded.list("/")), "\n")' \
    'assert(root_listing:match("/index%.html"))' \
    'assert(root_listing:match("/app%.css"))' \
    'assert(root_listing:match("/app%.js"))' \
    'assert(root_listing:match("/assets/logo%.txt"))' \
    'assert(root_listing:match("/templates/login%.html"))' \
    'local stat = assert(vectis.embedded.stat("/app.js"))' \
    'assert(stat.content_type == "application/javascript")' \
    'assert(assert(vectis.embedded.stat("/app.css")).content_type == "text/css")' \
    'assert(assert(vectis.embedded.stat("/assets/logo.txt")).content_type == "text/plain")' \
    'assert(assert(vectis.embedded.stat("/templates/login.html")).content_type == "text/html; charset=utf-8")' \
    'assert(assert(vectis.embedded.read("/assets/logo.txt")):match("VX packed logo"))' \
    'local logo_chunks = {}' \
    'for chunk in vectis.embedded.chunks("/assets/logo.txt", 4) do' \
    '  logo_chunks[#logo_chunks + 1] = chunk' \
    'end' \
    'assert(table.concat(logo_chunks) == "VX packed logo\n")' \
    'assert(assert(vectis.embedded.read("/templates/login.html")):match("packed%-login"))' \
    'assert(vectis.auth.store_init({ credentials_path = credentials_path, auth_state_path = auth_state_path }))' \
    'assert(vectis.auth.user_add({' \
    '  credentials_path = credentials_path,' \
    '  username = "packed-user@example.com",' \
    '  password = "packed-password",' \
    '  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",' \
    '  totp_label = "Vectis:packed-user@example.com",' \
    '  issuer = "Vectis",' \
    '}))' \
    'assert(vectis.auth.user_add({' \
    '  credentials_path = credentials_path,' \
    '  username = "packed-totp@example.com",' \
    '  password = "packed-password",' \
    '  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",' \
    '  totp_label = "Vectis:packed-totp@example.com",' \
    '  issuer = "Vectis",' \
    '}))' \
    'assert(vectis.auth.user_add({' \
    '  credentials_path = credentials_path,' \
    '  username = "packed-password-totp@example.com",' \
    '  password = "packed-password",' \
    '  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",' \
    '  totp_label = "Vectis:packed-password-totp@example.com",' \
    '  issuer = "Vectis",' \
    '}))' \
    'assert(vectis.auth.user_add({' \
    '  credentials_path = credentials_path,' \
    '  username = "packed-email-only@example.com",' \
    '  password = "packed-password",' \
    '}))' \
    'local server = assert(vectis.app.new({' \
    '  app_name = "vectis-packed-service-e2e",' \
    '  bind = "127.0.0.1",' \
    '  port = port,' \
    '  tls = { mode = "disabled" },' \
    '  lockd = {' \
    '    endpoints = { lockd_endpoint },' \
    '    client_bundle = "embedded",' \
    '    namespace = lockd_namespace,' \
    '  },' \
    '}))' \
    'assert(server:static_embedded({' \
    '  path_prefix = "/site",' \
    '  cache_control = "no-store",' \
    '}))' \
    'assert(server:webdav_embedded_site({' \
    '  path_prefix = "/dav",' \
    '  cache_dir = cache_dir,' \
    '  site_id = "packed-service-e2e",' \
    '  extract_policy = "repair",' \
    '  auth = {' \
    '    kind = "native",' \
    '    credentials_path = credentials_path,' \
    '    realm = "packed-e2e",' \
    '  },' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Login",' \
    '  time = 59,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '  email_smtp = {' \
    '    url = smtp_url,' \
    '    mail_from = "sender@example.test",' \
    '    allowed_domain = "example.test",' \
    '    timeout_ms = 5000,' \
    '    connect_timeout_ms = 2000,' \
    '  },' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-local",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Local Token Login",' \
    '  time = 59,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-limited",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Limited Token Login",' \
    '  time = 59,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '  email_token_max_attempts = 2,' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-email-only",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Email Only Login",' \
    '  time = 59,' \
    '  required_factors = { "email_token" },' \
    '  email_token_ttl_seconds = 300,' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-password-email",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Password Email Login",' \
    '  time = 59,' \
    '  required_factors = { "password", "email_token" },' \
    '  email_token_ttl_seconds = 300,' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-password",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Password Login",' \
    '  time = 59,' \
    '  required_factors = { "password" },' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-template",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed Embedded Template Login",' \
    '  time = 59,' \
    '  required_factors = { "password" },' \
    '  login_template_embedded_path = "/templates/login.html",' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-totp",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E TOTP Login",' \
    '  time = 59,' \
    '  required_factors = { "password", "totp" },' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-expired",' \
    '  credentials_path = credentials_path,' \
    '  auth_state_path = auth_state_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Expired Login",' \
    '  time = 360,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '}))' \
    'assert(server:auth_json({' \
    '  path = "/api/private",' \
    '  auth = {' \
    '    kind = "native",' \
    '    credentials_path = credentials_path,' \
    '    realm = "packed-e2e",' \
    '  },' \
    '  body = [[{"ok":true,"surface":"packed-api"}]],' \
    '}))' \
    'assert(server:consumer_service({' \
    '  queue = lockd_queue,' \
    '  owner = "packed-consumer",' \
    '  name = "packed-consumer",' \
    '  worker_count = 1,' \
    '  wait_seconds = 1,' \
    '  visibility_timeout_seconds = 30,' \
    '  processing_delay_seconds = 3,' \
    '  handler = {' \
    '    kind = "webdav_marker",' \
    '    cache_dir = cache_dir,' \
    '    site_id = "packed-service-e2e",' \
    '    processing_path = "/consumer-processing.txt",' \
    '    done_path = "/consumer-done.txt",' \
    '    processing_body = "processing\n",' \
    '    done_body = "handled\n",' \
    '  },' \
    '}))' \
    'assert(server:static_embedded({' \
    '  path_prefix = "/",' \
    '  cache_control = "no-store",' \
    '}))' \
    'assert(server:start())' \
    'local ready = assert(io.open(ready_path, "wb"))' \
    'assert(ready:write("ready\n"))' \
    'assert(ready:close())' \
    'assert(server:wait())' >"$packed_service_script"
  "$repo_root/build/debug/vectis" -a pack \
    --script "$packed_service_script" \
    --asset-dir "/:$packed_service_site" \
    --content-type-map "$packed_content_types" \
    --extract-mode repair \
    --lockd-bundle "$client_bundle" \
    --output "$packed_service"
  mkdir -p "$packed_service_docroot/assets"
  printf '%s\n' 'stale extracted css' >"$packed_service_docroot/app.css"
  printf '%s\n' 'preexisting mutable content' \
    >"$packed_service_docroot/user-created-before-repair.txt"
  start_server "lua packed webserver" "$packed_service_log" \
    env VECTIS_PACKED_SERVICE_PORT="$kore_packed_port" \
      VECTIS_PACKED_SERVICE_CREDENTIALS="$packed_service_credentials" \
      VECTIS_PACKED_SERVICE_AUTH_STATE="$packed_service_auth_state" \
      VECTIS_PACKED_SERVICE_CACHE="$packed_service_cache" \
      VECTIS_PACKED_SERVICE_LOCKD_ENDPOINT="$disk_endpoint" \
      VECTIS_PACKED_SERVICE_LOCKD_QUEUE="$packed_service_queue" \
      VECTIS_PACKED_SERVICE_LOCKD_NAMESPACE="$packed_service_namespace" \
      VECTIS_PACKED_SERVICE_READY="$packed_service_ready" \
      "$pack_smtp_harness" "$packed_service" "$packed_service_mailbox"
  count=0
  while [ ! -f "$packed_service_ready" ]; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for lua packed webserver app readiness" >&2
      sed 's/^/[lua-packed-webserver] /' "$packed_service_log" >&2
      return 1
    fi
    sleep 1
  done
  wait_for_http "http://127.0.0.1:$kore_packed_port/site/index.html" \
    "lua packed webserver" "$packed_service_log"
  if [ ! -f "$packed_service_docroot/index.html" ] ||
      [ ! -f "$packed_service_docroot/app.css" ] ||
      [ ! -f "$packed_service_docroot/app.js" ] ||
      [ ! -f "$packed_service_docroot/assets/logo.txt" ] ||
      [ ! -f "$packed_service_docroot/templates/login.html" ]; then
    printf '%s\n' "Packed WebDAV extracted docroot was not initialized at $packed_service_docroot" >&2
    find "$packed_service_cache" -maxdepth 5 -type f -print 2>/dev/null >&2 || true
    return 1
  fi
  body=$(cat "$packed_service_docroot/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected extracted packed index body: $body" >&2
      return 1
      ;;
  esac
  body=$(cat "$packed_service_docroot/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Unexpected extracted packed logo body: $body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/app.css")
  if [ "$body" != "body { color: #123456; }" ]; then
    printf '%s\n' "Packed WebDAV repair did not restore stale CSS: $body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/user-created-before-repair.txt")
  if [ "$body" != "preexisting mutable content" ]; then
    printf '%s\n' "Packed WebDAV repair did not preserve user-created file: $body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/templates/login.html")
  case "$body" in
    *'packed-login'*'username'*) ;;
    *)
      printf '%s\n' "Unexpected extracted packed template body: $body" >&2
      return 1
      ;;
  esac
  packed_static_index_body="$work_dir/packed-static-index.body"
  curl_body_or_log "$packed_service_log" "packed static index" \
    "$packed_static_index_body" --max-time 10 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/index.html"
  body=$(cat "$packed_static_index_body")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed static response: $body" >&2
      return 1
      ;;
  esac
  packed_root_static_index_body="$work_dir/packed-root-static-index.body"
  curl_body_or_log "$packed_service_log" "packed root static index" \
    "$packed_root_static_index_body" --max-time 10 -fsS \
    "http://127.0.0.1:$kore_packed_port/"
  body=$(cat "$packed_root_static_index_body")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed root static response: $body" >&2
      return 1
      ;;
  esac
  static_root_css_body="$work_dir/packed-root-css.body"
  curl_body_or_log "$packed_service_log" "packed root static css body" \
    "$static_root_css_body" --max-time 10 -fsS \
    "http://127.0.0.1:$kore_packed_port/app.css"
  body=$(cat "$static_root_css_body")
  if [ "$body" != "body { color: #123456; }" ]; then
    printf '%s\n' "Unexpected packed root CSS response: $body" >&2
    return 1
  fi
  static_head_headers="$work_dir/packed-static-head.headers"
  static_head_status=$(curl --max-time 3 -sS -D "$static_head_headers" \
    -o /dev/null -w '%{http_code}' --head \
    "http://127.0.0.1:$kore_packed_port/site/index.html")
  if [ "$static_head_status" != "200" ]; then
    printf '%s\n' "Unexpected packed static HEAD status: $static_head_status" >&2
    sed 's/^/[packed-static-head] /' "$static_head_headers" >&2
    return 1
  fi
  grep -qi '^content-type: text/html; charset=utf-8' "$static_head_headers" || {
    printf '%s\n' "Packed static HEAD did not include HTML content type" >&2
    sed 's/^/[packed-static-head] /' "$static_head_headers" >&2
    return 1
  }
  grep -qi '^etag:' "$static_head_headers" || {
    printf '%s\n' "Packed static HEAD did not include ETag" >&2
    sed 's/^/[packed-static-head] /' "$static_head_headers" >&2
    return 1
  }
  grep -qi '^cache-control: no-store' "$static_head_headers" || {
    printf '%s\n' "Packed static HEAD did not include cache-control" >&2
    sed 's/^/[packed-static-head] /' "$static_head_headers" >&2
    return 1
  }
  root_static_put_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X PUT --data 'blocked root write' \
    "http://127.0.0.1:$kore_packed_port/")
  if [ "$root_static_put_status" = "200" ] ||
      [ "$root_static_put_status" = "201" ] ||
      [ "$root_static_put_status" = "204" ]; then
    printf '%s\n' "Unexpected packed root static PUT status: $root_static_put_status" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed root static index after put" \
    --max-time 3 -fsS "http://127.0.0.1:$kore_packed_port/")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Packed root static asset changed after rejected PUT: $body" >&2
      return 1
      ;;
  esac
  for traversal_path in \
    "/../secret" \
    "/%2e%2e/secret" \
    "/%2E%2E/secret" \
    "/..%2fsecret"; do
    traversal_status=$(curl --path-as-is --max-time 3 -sS -o /dev/null \
      -w '%{http_code}' \
      "http://127.0.0.1:$kore_packed_port$traversal_path" || true)
    if [ "$traversal_status" != "400" ] &&
        [ "$traversal_status" != "404" ]; then
      printf '%s\n' "Unexpected packed root traversal status for $traversal_path: $traversal_status" >&2
      return 1
    fi
  done
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'local port = tonumber(assert(os.getenv("VECTIS_PACKED_HTTPS_PORT")))' \
    'local cert = assert(os.getenv("VECTIS_PACKED_HTTPS_CERT"))' \
    'assert(vectis.cert.generate_bundle({' \
    '  common_name = "localhost",' \
    '  ip_addresses = "127.0.0.1",' \
    '  output_bundle_path = cert,' \
    '  key_bits = 2048,' \
    '  valid_days = 1,' \
    '}))' \
    'local server = assert(vectis.app.new({' \
    '  app_name = "vectis-packed-https-e2e",' \
    '  bind = "127.0.0.1",' \
    '  port = port,' \
    '  tls = {' \
    '    mode = "manual",' \
    '    cert_key_bundle_path = cert,' \
    '    domain = "localhost",' \
    '  },' \
    '}))' \
    'assert(server:static_embedded({' \
    '  path_prefix = "/",' \
    '  cache_control = "max-age=30",' \
    '}))' \
    'assert(server:start())' \
    'assert(server:wait())' >"$packed_https_script"
  "$repo_root/build/debug/vectis" -a pack \
    --script "$packed_https_script" \
    --asset-dir "/:$packed_service_site" \
    --content-type-map "$packed_content_types" \
    --output "$packed_https"
  start_server "lua packed https" "$packed_https_log" \
    env VECTIS_PACKED_HTTPS_PORT="$kore_packed_https_port" \
      VECTIS_PACKED_HTTPS_CERT="$packed_https_cert" \
      "$packed_https"
  wait_for_https_insecure "https://localhost:$kore_packed_https_port/" \
    "lua packed https"
  body=$(curl_or_log "$packed_https_log" "packed https root" --max-time 3 -k -fsS \
    "https://localhost:$kore_packed_https_port/")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed HTTPS root response: $body" >&2
      return 1
      ;;
  esac
  curl_or_log "$packed_https_log" "packed https root headers" --max-time 3 \
    -k -fsSI "https://localhost:$kore_packed_https_port/" |
    grep -qi '^cache-control: max-age=30' || {
      printf '%s\n' "Packed HTTPS root response did not include cache-control" >&2
      return 1
    }
  printf '[e2e] lua acme mock issuance\n'
  start_server "mock acme provider" "$acme_mock_log" \
    "$acme_mock_provider" "$acme_mock_port"
  wait_for_http "http://127.0.0.1:$acme_mock_port/directory" \
    "mock acme provider"
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'local port = tonumber(assert(os.getenv("VECTIS_ACME_STATE_PORT")))' \
    'local cache_dir = assert(os.getenv("VECTIS_ACME_STATE_CACHE"))' \
    'local credentials_path = assert(os.getenv("VECTIS_ACME_STATE_CREDENTIALS"))' \
    'local provider = assert(os.getenv("VECTIS_ACME_STATE_PROVIDER"))' \
    'assert(vectis.auth.store_init({ credentials_path = credentials_path }))' \
    'local server = assert(vectis.app.new({' \
    '  app_name = "vectis-acme-state-e2e",' \
    '  bind = "127.0.0.1",' \
    '  port = port,' \
    '  tls = {' \
    '    mode = "acme",' \
    '    domains = { "acme.localhost.test" },' \
    '    email = "ops@example.test",' \
    '    provider = provider,' \
    '    cache_dir = cache_dir,' \
    '  },' \
    '}))' \
    'assert(server:json({' \
    '  path = "/probe",' \
    '  body = [[{"ok":true,"surface":"acme-state"}]],' \
    '}))' \
    'assert(server:start())' \
    'assert(server:wait())' >"$acme_state_script"
  start_server "lua acme state" "$acme_state_log" \
    env VECTIS_ACME_STATE_PORT="$kore_acme_port" \
      VECTIS_ACME_STATE_CACHE="$acme_state_cache" \
      VECTIS_ACME_STATE_CREDENTIALS="$acme_state_credentials" \
      VECTIS_ACME_STATE_PROVIDER="http://127.0.0.1:$acme_mock_port/directory" \
      "$repo_root/build/debug/vectis" "$acme_state_script"
  count=0
  while [ ! -f "$acme_state_cache/account-key.pem" ] ||
      [ ! -f "$acme_state_cache/certificates/acme.localhost.test/fullchain.pem" ]; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "ACME mock issuance did not complete under configured cache dir: $acme_state_cache" >&2
      find "$acme_state_cache" -maxdepth 4 -print 2>/dev/null >&2 || true
      sed 's/^/[lua-acme-state] /' "$acme_state_log" >&2
      sed 's/^/[acme-mock] /' "$acme_mock_log" >&2
      return 1
    fi
    sleep 1
  done
  grep -q 'BEGIN CERTIFICATE' \
    "$acme_state_cache/certificates/acme.localhost.test/fullchain.pem" || {
      printf '%s\n' "ACME mock fullchain did not contain a PEM certificate" >&2
      return 1
    }
  acme_probe=$(curl_or_log "$acme_state_log" "mock acme https probe" \
    --resolve "acme.localhost.test:$kore_acme_port:127.0.0.1" \
    --max-time 3 -k -fsS \
    "https://acme.localhost.test:$kore_acme_port/probe")
  case "$acme_probe" in
    *'"surface":"acme-state"'*) ;;
    *)
      printf '%s\n' "Unexpected ACME HTTPS probe response: $acme_probe" >&2
      return 1
      ;;
  esac
  range_headers="$work_dir/packed-static-range.headers"
  range_body="$work_dir/packed-static-range.body"
  curl_head_or_log "$packed_service_log" "packed static app.js headers" \
    "$range_headers" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.js"
  grep -qi '^content-type: application/javascript' "$range_headers" || {
    printf '%s\n' "Packed static content type was not application/javascript" >&2
    sed 's/^/[packed-static-app-js] /' "$range_headers" >&2
    return 1
  }
  range_status=$(curl --max-time 3 -sS -D "$range_headers" -o "$range_body" \
    -w '%{http_code}' -H 'Range: bytes=0-5' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$range_status" != "206" ]; then
    printf '%s\n' "Unexpected packed static range status: $range_status" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  body=$(cat "$range_body")
  if [ "$body" != "window" ]; then
    printf '%s\n' "Unexpected packed static range body: $body" >&2
    return 1
  fi
  grep -qi '^content-range: bytes 0-5/' "$range_headers" || {
    printf '%s\n' "Packed static range response did not include Content-Range" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  grep -qi '^accept-ranges: bytes' "$range_headers" || {
    printf '%s\n' "Packed static range response did not include Accept-Ranges" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  range_status=$(curl --max-time 3 -sS -D "$range_headers" -o /dev/null \
    -w '%{http_code}' --head -H 'Range: bytes=0-5' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$range_status" != "206" ]; then
    printf '%s\n' "Unexpected packed static HEAD range status: $range_status" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  fi
  grep -qi '^content-range: bytes 0-5/' "$range_headers" || {
    printf '%s\n' "Packed static HEAD range did not include Content-Range" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  grep -qi '^accept-ranges: bytes' "$range_headers" || {
    printf '%s\n' "Packed static HEAD range did not include Accept-Ranges" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  grep -qi '^etag:' "$range_headers" || {
    printf '%s\n' "Packed static HEAD range did not include ETag" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  range_status=$(curl --max-time 3 -sS -D "$range_headers" -o "$range_body" \
    -w '%{http_code}' -H 'Range: bytes=-6' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$range_status" != "206" ]; then
    printf '%s\n' "Unexpected packed static suffix range status: $range_status" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  body=$(cat "$range_body")
  if [ "$body" != "true;" ]; then
    printf '%s\n' "Unexpected packed static suffix range body: $body" >&2
    return 1
  fi
  range_status=$(curl --max-time 3 -sS -D "$range_headers" -o "$range_body" \
    -w '%{http_code}' -H 'Range: bytes=999-1000' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$range_status" != "416" ]; then
    printf '%s\n' "Unexpected packed static unsatisfiable range status: $range_status" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  if [ -s "$range_body" ]; then
    printf '%s\n' "Packed static unsatisfiable range returned a body" >&2
    cat "$range_body" >&2
    return 1
  fi
  grep -qi '^content-range: bytes \*/' "$range_headers" || {
    printf '%s\n' "Packed static unsatisfiable range did not include Content-Range" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  range_status=$(curl --max-time 3 -sS -D "$range_headers" -o "$range_body" \
    -w '%{http_code}' -H 'Range: bytes=0-1,4-5' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$range_status" != "416" ]; then
    printf '%s\n' "Unexpected packed static multi-range status: $range_status" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  if [ -s "$range_body" ]; then
    printf '%s\n' "Packed static multi-range returned a body" >&2
    cat "$range_body" >&2
    return 1
  fi
  grep -qi '^content-range: bytes \*/' "$range_headers" || {
    printf '%s\n' "Packed static multi-range did not include Content-Range" >&2
    sed 's/^/[packed-range-header] /' "$range_headers" >&2
    return 1
  }
  curl --max-time 3 -sS -D "$range_headers" -o /dev/null \
    "http://127.0.0.1:$kore_packed_port/site/app.js"
  etag=$(awk 'BEGIN{IGNORECASE=1} /^etag:/ {sub(/^[^:]*:[ \t]*/, ""); sub(/\r$/, ""); print; exit}' "$range_headers")
  if [ -z "$etag" ]; then
    printf '%s\n' "Packed static app.js response did not include an ETag" >&2
    sed 's/^/[packed-etag-header] /' "$range_headers" >&2
    return 1
  fi
  if_range_status=$(curl --max-time 3 -sS -D "$range_headers" \
    -o "$range_body" -w '%{http_code}' -H 'Range: bytes=0-5' \
    -H "If-Range: $etag" \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$if_range_status" != "206" ]; then
    printf '%s\n' "Unexpected packed static If-Range status: $if_range_status" >&2
    sed 's/^/[packed-if-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  body=$(cat "$range_body")
  if [ "$body" != "window" ]; then
    printf '%s\n' "Unexpected packed static If-Range body: $body" >&2
    return 1
  fi
  grep -qi '^content-range: bytes 0-5/' "$range_headers" || {
    printf '%s\n' "Packed static If-Range response did not include Content-Range" >&2
    sed 's/^/[packed-if-range-header] /' "$range_headers" >&2
    return 1
  }
  if_range_status=$(curl --max-time 3 -sS -D "$range_headers" \
    -o "$range_body" -w '%{http_code}' -H 'Range: bytes=0-5' \
    -H 'If-Range: "different"' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$if_range_status" != "200" ]; then
    printf '%s\n' "Unexpected packed static stale If-Range status: $if_range_status" >&2
    sed 's/^/[packed-if-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  body=$(cat "$range_body")
  if [ "$body" != "window.vectisPackedService = true;" ]; then
    printf '%s\n' "Unexpected packed static stale If-Range body: $body" >&2
    return 1
  fi
  if grep -qi '^content-range:' "$range_headers"; then
    printf '%s\n' "Packed static stale If-Range response included Content-Range" >&2
    sed 's/^/[packed-if-range-header] /' "$range_headers" >&2
    return 1
  fi
  : >"$range_body"
  not_modified_status=$(curl --max-time 3 -sS -D "$range_headers" \
    -o "$range_body" -w '%{http_code}' -H "If-None-Match: $etag" \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$not_modified_status" != "304" ]; then
    printf '%s\n' "Unexpected packed static If-None-Match status: $not_modified_status" >&2
    sed 's/^/[packed-not-modified-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  if [ -s "$range_body" ]; then
    printf '%s\n' "Packed static If-None-Match response returned a body" >&2
    cat "$range_body" >&2
    return 1
  fi
  grep -qi '^etag:' "$range_headers" || {
    printf '%s\n' "Packed static If-None-Match response did not keep ETag" >&2
    sed 's/^/[packed-not-modified-header] /' "$range_headers" >&2
    return 1
  }
  : >"$range_body"
  not_modified_status=$(curl --max-time 3 -sS -D "$range_headers" \
    -o "$range_body" -w '%{http_code}' -H "If-None-Match: $etag" \
    -H 'Range: bytes=0-5' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$not_modified_status" != "304" ]; then
    printf '%s\n' "Unexpected packed static If-None-Match+Range status: $not_modified_status" >&2
    sed 's/^/[packed-not-modified-range-header] /' "$range_headers" >&2
    cat "$range_body" >&2
    return 1
  fi
  if [ -s "$range_body" ]; then
    printf '%s\n' "Packed static If-None-Match+Range response returned a body" >&2
    cat "$range_body" >&2
    return 1
  fi
  if grep -qi '^content-range:' "$range_headers"; then
    printf '%s\n' "Packed static If-None-Match+Range response included Content-Range" >&2
    sed 's/^/[packed-not-modified-range-header] /' "$range_headers" >&2
    return 1
  fi
  : >"$range_body"
  not_modified_status=$(curl --max-time 3 -sS -D "$range_headers" \
    -o /dev/null -w '%{http_code}' --head -H "If-None-Match: $etag" \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$not_modified_status" != "304" ]; then
    printf '%s\n' "Unexpected packed static HEAD If-None-Match status: $not_modified_status" >&2
    sed 's/^/[packed-not-modified-header] /' "$range_headers" >&2
    return 1
  fi
  grep -qi '^etag:' "$range_headers" || {
    printf '%s\n' "Packed static HEAD If-None-Match response did not keep ETag" >&2
    sed 's/^/[packed-not-modified-header] /' "$range_headers" >&2
    return 1
  }
  : >"$range_body"
  not_modified_status=$(curl --max-time 3 -sS -D "$range_headers" \
    -o /dev/null -w '%{http_code}' --head -H "If-None-Match: $etag" \
    -H 'Range: bytes=0-5' \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$not_modified_status" != "304" ]; then
    printf '%s\n' "Unexpected packed static HEAD If-None-Match+Range status: $not_modified_status" >&2
    sed 's/^/[packed-not-modified-range-header] /' "$range_headers" >&2
    return 1
  fi
  if grep -qi '^content-range:' "$range_headers"; then
    printf '%s\n' "Packed static HEAD If-None-Match+Range response included Content-Range" >&2
    sed 's/^/[packed-not-modified-range-header] /' "$range_headers" >&2
    return 1
  fi
  curl_head_or_log "$packed_service_log" "packed static app.css content type" \
    "$range_headers" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.css"
  grep -qi '^content-type: text/css' "$range_headers" || {
      printf '%s\n' "Packed static CSS content type was not text/css" >&2
      sed 's/^/[packed-static-app-css] /' "$range_headers" >&2
      return 1
    }
  curl_head_or_log "$packed_service_log" "packed static app.css etag" \
    "$range_headers" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.css"
  grep -qi '^etag:' "$range_headers" || {
      printf '%s\n' "Packed static CSS response did not include an ETag" >&2
      sed 's/^/[packed-static-app-css] /' "$range_headers" >&2
      return 1
    }
  curl_head_or_log "$packed_service_log" "packed static app.css cache-control" \
    "$range_headers" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.css"
  grep -qi '^cache-control: no-store' "$range_headers" || {
      printf '%s\n' "Packed static CSS response did not include cache-control" >&2
      sed 's/^/[packed-static-app-css] /' "$range_headers" >&2
      return 1
    }
  body=$(curl_or_log "$packed_service_log" "packed static css body" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.css")
  if [ "$body" != "body { color: #123456; }" ]; then
    printf '%s\n' "Unexpected packed CSS response: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed static javascript body" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.js")
  if [ "$body" != "window.vectisPackedService = true;" ]; then
    printf '%s\n' "Unexpected packed JavaScript response: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed static logo" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Unexpected packed logo response: $body" >&2
    return 1
  fi
  curl_head_or_log "$packed_service_log" "packed static template headers" \
    "$range_headers" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/templates/login.html"
  grep -qi '^content-type: text/html; charset=utf-8' "$range_headers" || {
      printf '%s\n' "Packed template content type was not text/html; charset=utf-8" >&2
      sed 's/^/[packed-static-template] /' "$range_headers" >&2
      return 1
    }
  body=$(curl_or_log "$packed_service_log" "packed static template" --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/templates/login.html")
  case "$body" in
    *'packed-login'*'username'*) ;;
    *)
      printf '%s\n' "Unexpected packed template response: $body" >&2
      return 1
      ;;
  esac
  for traversal_path in \
    "/site/../secret" \
    "/site/%2e%2e/secret" \
    "/site/%2E%2E/secret" \
    "/site/..%2fsecret"; do
    traversal_status=$(curl --path-as-is --max-time 3 -sS -o /dev/null -w '%{http_code}' \
      "http://127.0.0.1:$kore_packed_port$traversal_path" || true)
    if [ "$traversal_status" != "400" ] && [ "$traversal_status" != "404" ]; then
      printf '%s\n' "Unexpected packed traversal status for $traversal_path: $traversal_status" >&2
      return 1
    fi
  done
  static_put_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X PUT --data 'blocked' \
    "http://127.0.0.1:$kore_packed_port/site/index.html")
  if [ "$static_put_status" = "200" ] ||
      [ "$static_put_status" = "201" ] ||
      [ "$static_put_status" = "204" ]; then
    printf '%s\n' "Unexpected packed static PUT status: $static_put_status" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-auth-login.headers"
  body=$(curl_or_log "$packed_service_log" "packed auth login" \
    --max-time 3 -fsS -D "$auth_headers" \
    "http://127.0.0.1:$kore_packed_port/auth/login")
  assert_no_store_headers "$auth_headers" "packed auth login"
  case "$body" in
    *'Packed E2E Login'*'action="/auth/continue"'*) ;;
    *)
      printf '%s\n' "Unexpected packed auth login response: $body" >&2
      return 1
      ;;
  esac
  case "$body" in
    *'action="/auth/webdav-key"'*)
      printf '%s\n' "Packed auth login form should use the continue action" >&2
      printf '%s\n' "$body" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-auth-template-login.headers"
  embedded_template_body=$(curl_or_log "$packed_service_log" \
    "packed embedded-template auth login" \
    --max-time 3 -fsS -D "$auth_headers" \
    "http://127.0.0.1:$kore_packed_port/auth-template/login")
  assert_no_store_headers "$auth_headers" "packed embedded-template auth login"
  for template_fragment in \
    'id="packed-login"' \
    'Packed Embedded Template Login' \
    'data-realm="packed-e2e"' \
    'action="/auth-template/continue"' \
    'href="/auth-template/webdav-key"'; do
    case "$embedded_template_body" in
      *"$template_fragment"*) ;;
      *)
        printf '%s\n' "Packed embedded-template auth login missing fragment: $template_fragment" >&2
        printf '%s\n' "$embedded_template_body" >&2
        return 1
        ;;
    esac
  done
  case "$embedded_template_body" in
    *'{{'*|*'}}'*)
      printf '%s\n' "Packed embedded-template auth login left placeholders unexpanded" >&2
      printf '%s\n' "$embedded_template_body" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-default-email-login.headers"
  default_email_login_body=$(curl_or_log "$packed_service_log" \
    "packed default email-token login" \
    --max-time 3 -fsS -D "$auth_headers" \
    "http://127.0.0.1:$kore_packed_port/auth-local/login")
  assert_no_store_headers "$auth_headers" "packed default email-token login"
  for login_fragment in \
    'action="/auth-local/continue"' \
    'name="password"' \
    'name="totp_code"' \
    'name="email_transaction_id"' \
    'name="email_token"' \
    'action="/auth-local/email-token"' \
    'name="email"'; do
    case "$default_email_login_body" in
      *"$login_fragment"*) ;;
      *)
        printf '%s\n' \
          "Packed default email-token login missing fragment: $login_fragment" >&2
        printf '%s\n' "$default_email_login_body" >&2
        return 1
        ;;
    esac
  done
  auth_headers="$work_dir/packed-default-email-only-login.headers"
  default_email_only_login_body=$(curl_or_log "$packed_service_log" \
    "packed default email-only login" \
    --max-time 3 -fsS -D "$auth_headers" \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/login")
  assert_no_store_headers "$auth_headers" "packed default email-only login"
  for login_fragment in \
    'action="/auth-email-only/continue"' \
    'name="email_transaction_id"' \
    'name="email_token"' \
    'action="/auth-email-only/email-token"' \
    'name="email"'; do
    case "$default_email_only_login_body" in
      *"$login_fragment"*) ;;
      *)
        printf '%s\n' \
          "Packed default email-only login missing fragment: $login_fragment" >&2
        printf '%s\n' "$default_email_only_login_body" >&2
        return 1
        ;;
    esac
  done
  for forbidden_fragment in \
    'name="password"' \
    'name="totp_code"'; do
    case "$default_email_only_login_body" in
      *"$forbidden_fragment"*)
        printf '%s\n' \
          "Packed default email-only login unexpectedly rendered: $forbidden_fragment" >&2
        printf '%s\n' "$default_email_only_login_body" >&2
        return 1
        ;;
    esac
  done
  auth_headers="$work_dir/packed-default-password-login.headers"
  default_password_login_body=$(curl_or_log "$packed_service_log" \
    "packed default password login" \
    --max-time 3 -fsS -D "$auth_headers" \
    "http://127.0.0.1:$kore_packed_port/auth-password/login")
  assert_no_store_headers "$auth_headers" "packed default password login"
  case "$default_password_login_body" in
    *'action="/auth-password/continue"'*) ;;
    *)
      printf '%s\n' \
        "Packed default password login missing continue form action" >&2
      printf '%s\n' "$default_password_login_body" >&2
      return 1
      ;;
  esac
  for login_fragment in \
    'name="password"' \
    'name="totp_code"'; do
    case "$default_password_login_body" in
      *"$login_fragment"*) ;;
      *)
        printf '%s\n' \
          "Packed default password login missing fragment: $login_fragment" >&2
        printf '%s\n' "$default_password_login_body" >&2
        return 1
        ;;
    esac
  done
  for forbidden_fragment in \
    'name="email_transaction_id"' \
    'name="email_token"' \
    'action="/auth-password/email-token"'; do
    case "$default_password_login_body" in
      *"$forbidden_fragment"*)
        printf '%s\n' \
          "Packed default password login unexpectedly rendered: $forbidden_fragment" >&2
        printf '%s\n' "$default_password_login_body" >&2
        return 1
        ;;
    esac
  done
  assert_packed_logout_requires_authorization "packed-auth" "/auth"
  assert_packed_logout_requires_authorization "packed-auth-password" \
    "/auth-password"
  auth_headers="$work_dir/packed-password-login-post.headers"
  password_login_post_response=$(curl_or_log "$packed_service_log" \
    "packed password POST login" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-password/login")
  assert_no_store_headers "$auth_headers" "packed password POST login"
  password_login_post_client_id=$(printf '%s\n' "$password_login_post_response" |
    sed -n 's/^client_id=//p')
  password_login_post_client_secret=$(printf '%s\n' "$password_login_post_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$password_login_post_client_id" ] ||
      [ -z "$password_login_post_client_secret" ]; then
    printf '%s\n' "Packed password POST login did not issue credentials" >&2
    printf '%s\n' "$password_login_post_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" \
    "packed password POST login guarded api" \
    --max-time 3 -fsS \
    -u "$password_login_post_client_id:$password_login_post_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed password POST login guarded API response: $body" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-password-post-login" \
    "/auth-password" "$password_login_post_client_id" \
    "$password_login_post_client_secret"
  auth_headers="$work_dir/packed-browser-continue-start.headers"
  browser_continue_start_response=$(curl_or_log "$packed_service_log" \
    "packed browser continue start" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth/continue")
  assert_no_store_headers "$auth_headers" "packed browser continue start"
  browser_pending_id=$(printf '%s\n' "$browser_continue_start_response" |
    sed -n 's/^pending_transaction_id=//p')
  if [ -z "$browser_pending_id" ]; then
    printf '%s\n' "Packed browser continue did not return a pending transaction" >&2
    printf '%s\n' "$browser_continue_start_response" >&2
    return 1
  fi
  assert_packed_auth_state_record "packed browser pending" "$browser_pending_id"
  case "$browser_continue_start_response" in
    *'totp_required=1'*) ;;
    *)
      printf '%s\n' "Packed browser continue did not require TOTP" >&2
      printf '%s\n' "$browser_continue_start_response" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-password-only.headers"
  password_only_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&password=packed-password&totp_code=287082' \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  if [ "$password_only_status" = "200" ]; then
    printf '%s\n' "Packed auth issued WebDAV key without email token" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed password-only auth"
  auth_headers="$work_dir/packed-no-totp-email-token.headers"
  no_totp_email_token_response=$(curl_or_log "$packed_service_log" \
    "packed no-totp email token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&email=packed-email-only%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  assert_no_store_headers "$auth_headers" "packed no-totp email token"
  no_totp_email_transaction_id=$(printf '%s\n' "$no_totp_email_token_response" |
    sed -n 's/^transaction_id=//p')
  no_totp_email_token=$(printf '%s\n' "$no_totp_email_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$no_totp_email_transaction_id" ] || [ -z "$no_totp_email_token" ]; then
    printf '%s\n' "Packed local auth did not expose a no-TOTP email token" >&2
    printf '%s\n' "$no_totp_email_token_response" >&2
    return 1
  fi
  assert_packed_auth_state_record "packed no-totp email token" \
    "$no_totp_email_transaction_id"
  auth_headers="$work_dir/packed-no-totp-webdav-key.headers"
  no_totp_key_response=$(curl_or_log "$packed_service_log" \
    "packed no-totp webdav key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&password=packed-password&email_transaction_id=$no_totp_email_transaction_id&email_token=$no_totp_email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-local/webdav-key")
  assert_no_store_headers "$auth_headers" "packed no-totp webdav key"
  no_totp_client_id=$(printf '%s\n' "$no_totp_key_response" |
    sed -n 's/^client_id=//p')
  no_totp_client_secret=$(printf '%s\n' "$no_totp_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$no_totp_client_id" ] || [ -z "$no_totp_client_secret" ]; then
    printf '%s\n' "Packed no-TOTP auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$no_totp_key_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed no-totp guarded api" \
    --max-time 3 -fsS -u "$no_totp_client_id:$no_totp_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed no-TOTP guarded API response: $body" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-require-email-token" \
    "/auth-local" "$no_totp_client_id" "$no_totp_client_secret"
  auth_headers="$work_dir/packed-email-only-unknown.headers"
  email_only_unknown_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o "$work_dir/packed-email-only-unknown.txt" -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-unknown%40example.com&email=packed-unknown%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/email-token")
  email_only_unknown_body=$(cat "$work_dir/packed-email-only-unknown.txt")
  if [ "$email_only_unknown_status" != "401" ]; then
    printf '%s\n' "Packed email-only auth returned unexpected unknown-user status: $email_only_unknown_status" >&2
    printf '%s\n' "$email_only_unknown_body" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed email-only unknown user"
  if [ "$email_only_unknown_body" != "login failed" ]; then
    printf '%s\n' "Packed email-only auth returned unexpected unknown-user body: $email_only_unknown_body" >&2
    return 1
  fi
  case "$email_only_unknown_body" in
    *transaction_id=*)
      printf '%s\n' "Packed email-only auth exposed transaction id for unknown user" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-email-only-token.headers"
  email_only_token_response=$(curl_or_log "$packed_service_log" \
    "packed email-only token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&email=packed-email-only%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/email-token")
  assert_no_store_headers "$auth_headers" "packed email-only token"
  email_only_transaction_id=$(printf '%s\n' "$email_only_token_response" |
    sed -n 's/^transaction_id=//p')
  email_only_token=$(printf '%s\n' "$email_only_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$email_only_transaction_id" ] || [ -z "$email_only_token" ]; then
    printf '%s\n' "Packed email-only auth did not expose an email token" >&2
    printf '%s\n' "$email_only_token_response" >&2
    return 1
  fi
  assert_packed_auth_state_record "packed email-only token" \
    "$email_only_transaction_id"
  auth_headers="$work_dir/packed-email-only-missing-user.headers"
  email_only_missing_user_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o "$work_dir/packed-email-only-missing-user.txt" -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "email_transaction_id=$email_only_transaction_id&email_token=$email_only_token" \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/webdav-key")
  email_only_missing_user_body=$(cat "$work_dir/packed-email-only-missing-user.txt")
  if [ "$email_only_missing_user_status" != "400" ]; then
    printf '%s\n' "Packed email-only auth returned unexpected missing-user status: $email_only_missing_user_status" >&2
    printf '%s\n' "$email_only_missing_user_body" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed email-only missing user"
  if [ "$email_only_missing_user_body" != "username is required" ]; then
    printf '%s\n' "Packed email-only auth returned unexpected missing-user body: $email_only_missing_user_body" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-email-only-webdav-key.headers"
  email_only_key_response=$(curl_or_log "$packed_service_log" \
    "packed email-only webdav key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&email_transaction_id=$email_only_transaction_id&email_token=$email_only_token" \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/webdav-key")
  assert_no_store_headers "$auth_headers" "packed email-only webdav key"
  email_only_client_id=$(printf '%s\n' "$email_only_key_response" |
    sed -n 's/^client_id=//p')
  email_only_client_secret=$(printf '%s\n' "$email_only_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$email_only_client_id" ] || [ -z "$email_only_client_secret" ]; then
    printf '%s\n' "Packed email-only auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$email_only_key_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed email-only guarded api" \
    --max-time 3 -fsS -u "$email_only_client_id:$email_only_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed email-only guarded API response: $body" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-email-only" "/auth-email-only" \
    "$email_only_client_id" "$email_only_client_secret"
  auth_headers="$work_dir/packed-email-only-continue-token.headers"
  email_only_continue_token_response=$(curl_or_log "$packed_service_log" \
    "packed email-only continue token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&email=packed-email-only%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/email-token")
  assert_no_store_headers "$auth_headers" "packed email-only continue token"
  email_only_continue_transaction_id=$(printf '%s\n' "$email_only_continue_token_response" |
    sed -n 's/^transaction_id=//p')
  email_only_continue_token=$(printf '%s\n' "$email_only_continue_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$email_only_continue_transaction_id" ] ||
      [ -z "$email_only_continue_token" ]; then
    printf '%s\n' "Packed email-only continue auth did not expose an email token" >&2
    printf '%s\n' "$email_only_continue_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-email-only-continue-key.headers"
  email_only_continue_response=$(curl_or_log "$packed_service_log" \
    "packed email-only continue key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&email_transaction_id=$email_only_continue_transaction_id&email_token=$email_only_continue_token" \
    "http://127.0.0.1:$kore_packed_port/auth-email-only/continue")
  assert_no_store_headers "$auth_headers" "packed email-only continue key"
  email_only_continue_client_id=$(printf '%s\n' "$email_only_continue_response" |
    sed -n 's/^client_id=//p')
  email_only_continue_client_secret=$(printf '%s\n' "$email_only_continue_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$email_only_continue_client_id" ] ||
      [ -z "$email_only_continue_client_secret" ]; then
    printf '%s\n' "Packed email-only continue auth did not issue credentials" >&2
    printf '%s\n' "$email_only_continue_response" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-email-only-continue" \
    "/auth-email-only" "$email_only_continue_client_id" \
    "$email_only_continue_client_secret"
  auth_headers="$work_dir/packed-password-email-start.headers"
  password_email_start_response=$(curl_or_log "$packed_service_log" \
    "packed password-email start" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/webdav-key")
  assert_no_store_headers "$auth_headers" "packed password-email start"
  password_email_pending_id=$(printf '%s\n' "$password_email_start_response" |
    sed -n 's/^pending_transaction_id=//p')
  if [ -z "$password_email_pending_id" ]; then
    printf '%s\n' "Packed password-email auth did not return a pending transaction" >&2
    printf '%s\n' "$password_email_start_response" >&2
    return 1
  fi
  assert_packed_auth_state_record "packed password-email pending" \
    "$password_email_pending_id"
  case "$password_email_start_response" in
    *'totp_required=0'*) ;;
    *)
      printf '%s\n' "Packed password-email auth unexpectedly required TOTP" >&2
      printf '%s\n' "$password_email_start_response" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-password-email-token.headers"
  password_email_token_response=$(curl_or_log "$packed_service_log" \
    "packed password-email token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&email=packed-email-only%40example.test&pending_transaction_id=$password_email_pending_id" \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/email-token")
  assert_no_store_headers "$auth_headers" "packed password-email token"
  password_email_transaction_id=$(printf '%s\n' "$password_email_token_response" |
    sed -n 's/^transaction_id=//p')
  password_email_token=$(printf '%s\n' "$password_email_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$password_email_transaction_id" ] || [ -z "$password_email_token" ]; then
    printf '%s\n' "Packed password-email auth did not expose an email token" >&2
    printf '%s\n' "$password_email_token_response" >&2
    return 1
  fi
  assert_packed_auth_state_record "packed password-email token" \
    "$password_email_transaction_id"
  auth_headers="$work_dir/packed-password-email-wrong-pending-start.headers"
  password_email_wrong_pending_response=$(curl_or_log "$packed_service_log" \
    "packed password-email wrong-pending start" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/webdav-key")
  assert_no_store_headers "$auth_headers" "packed password-email wrong-pending start"
  password_email_wrong_pending_id=$(printf '%s\n' "$password_email_wrong_pending_response" |
    sed -n 's/^pending_transaction_id=//p')
  if [ -z "$password_email_wrong_pending_id" ] ||
      [ "$password_email_wrong_pending_id" = "$password_email_pending_id" ]; then
    printf '%s\n' "Packed password-email auth did not create a distinct wrong-pending transaction" >&2
    printf '%s\n' "$password_email_wrong_pending_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-password-email-wrong-pending.headers"
  password_email_wrong_pending_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o "$work_dir/packed-password-email-wrong-pending.txt" -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&pending_transaction_id=$password_email_wrong_pending_id&email_transaction_id=$password_email_transaction_id&email_token=$password_email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/webdav-key")
  password_email_wrong_pending_body=$(cat "$work_dir/packed-password-email-wrong-pending.txt")
  if [ "$password_email_wrong_pending_status" != "401" ]; then
    printf '%s\n' "Packed password-email auth returned unexpected wrong-pending status: $password_email_wrong_pending_status" >&2
    printf '%s\n' "$password_email_wrong_pending_body" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed password-email wrong pending"
  if [ "$password_email_wrong_pending_body" != "login failed" ]; then
    printf '%s\n' "Packed password-email auth returned unexpected wrong-pending body: $password_email_wrong_pending_body" >&2
    return 1
  fi
  case "$password_email_wrong_pending_body" in
    *client_id=*|*client_secret=*)
      printf '%s\n' "Packed password-email auth exposed credentials for wrong-pending token" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-password-email-webdav-key.headers"
  password_email_key_response=$(curl_or_log "$packed_service_log" \
    "packed password-email webdav key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&pending_transaction_id=$password_email_pending_id&email_transaction_id=$password_email_transaction_id&email_token=$password_email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/webdav-key")
  assert_no_store_headers "$auth_headers" "packed password-email webdav key"
  password_email_client_id=$(printf '%s\n' "$password_email_key_response" |
    sed -n 's/^client_id=//p')
  password_email_client_secret=$(printf '%s\n' "$password_email_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$password_email_client_id" ] || [ -z "$password_email_client_secret" ]; then
    printf '%s\n' "Packed password-email auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$password_email_key_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed password-email guarded api" \
    --max-time 3 -fsS -u "$password_email_client_id:$password_email_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed password-email guarded API response: $body" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-password-email" \
    "/auth-password-email" "$password_email_client_id" \
    "$password_email_client_secret"
  auth_headers="$work_dir/packed-password-email-continue-start.headers"
  password_email_continue_start_response=$(curl_or_log "$packed_service_log" \
    "packed password-email continue start" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/continue")
  assert_no_store_headers "$auth_headers" "packed password-email continue start"
  password_email_continue_pending_id=$(printf '%s\n' "$password_email_continue_start_response" |
    sed -n 's/^pending_transaction_id=//p')
  if [ -z "$password_email_continue_pending_id" ]; then
    printf '%s\n' "Packed password-email continue auth did not return pending transaction" >&2
    printf '%s\n' "$password_email_continue_start_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-password-email-continue-token.headers"
  password_email_continue_token_response=$(curl_or_log "$packed_service_log" \
    "packed password-email continue token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&email=packed-email-only%40example.test&pending_transaction_id=$password_email_continue_pending_id" \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/email-token")
  assert_no_store_headers "$auth_headers" "packed password-email continue token"
  password_email_continue_transaction_id=$(printf '%s\n' "$password_email_continue_token_response" |
    sed -n 's/^transaction_id=//p')
  password_email_continue_token=$(printf '%s\n' "$password_email_continue_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$password_email_continue_transaction_id" ] ||
      [ -z "$password_email_continue_token" ]; then
    printf '%s\n' "Packed password-email continue auth did not expose an email token" >&2
    printf '%s\n' "$password_email_continue_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-password-email-continue-key.headers"
  password_email_continue_response=$(curl_or_log "$packed_service_log" \
    "packed password-email continue key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&pending_transaction_id=$password_email_continue_pending_id&email_transaction_id=$password_email_continue_transaction_id&email_token=$password_email_continue_token" \
    "http://127.0.0.1:$kore_packed_port/auth-password-email/continue")
  assert_no_store_headers "$auth_headers" "packed password-email continue key"
  password_email_continue_client_id=$(printf '%s\n' "$password_email_continue_response" |
    sed -n 's/^client_id=//p')
  password_email_continue_client_secret=$(printf '%s\n' "$password_email_continue_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$password_email_continue_client_id" ] ||
      [ -z "$password_email_continue_client_secret" ]; then
    printf '%s\n' "Packed password-email continue auth did not issue credentials" >&2
    printf '%s\n' "$password_email_continue_response" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-password-email-continue" \
    "/auth-password-email" "$password_email_continue_client_id" \
    "$password_email_continue_client_secret"
  auth_headers="$work_dir/packed-password-webdav-key.headers"
  password_key_response=$(curl_or_log "$packed_service_log" \
    "packed password webdav key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-password/webdav-key")
  assert_no_store_headers "$auth_headers" "packed password webdav key"
  password_client_id=$(printf '%s\n' "$password_key_response" |
    sed -n 's/^client_id=//p')
  password_client_secret=$(printf '%s\n' "$password_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$password_client_id" ] || [ -z "$password_client_secret" ]; then
    printf '%s\n' "Packed password auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$password_key_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed password guarded api" \
    --max-time 3 -fsS -u "$password_client_id:$password_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed password guarded API response: $body" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-password" "/auth-password" \
    "$password_client_id" "$password_client_secret"
  auth_headers="$work_dir/packed-password-totp-pending.headers"
  password_totp_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o "$work_dir/packed-password-totp-pending.txt" -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-password-totp%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-password/webdav-key")
  password_totp_body=$(cat "$work_dir/packed-password-totp-pending.txt")
  if [ "$password_totp_status" != "202" ]; then
    printf '%s\n' "Packed password auth returned unexpected TOTP-pending status: $password_totp_status" >&2
    printf '%s\n' "$password_totp_body" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed password TOTP pending"
  case "$password_totp_body" in
    *'totp_required=1'*) ;;
    *)
      printf '%s\n' "Packed password auth did not request TOTP continuation" >&2
      printf '%s\n' "$password_totp_body" >&2
      return 1
      ;;
  esac
  case "$password_totp_body" in
    *client_id=*|*client_secret=*)
      printf '%s\n' "Packed password auth exposed credentials before TOTP" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-totp-start.headers"
  totp_start_response=$(curl_or_log "$packed_service_log" \
    "packed totp start" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-totp%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-totp/webdav-key")
  assert_no_store_headers "$auth_headers" "packed totp start"
  totp_pending_id=$(printf '%s\n' "$totp_start_response" |
    sed -n 's/^pending_transaction_id=//p')
  if [ -z "$totp_pending_id" ]; then
    printf '%s\n' "Packed TOTP auth did not return a pending transaction" >&2
    printf '%s\n' "$totp_start_response" >&2
    return 1
  fi
  case "$totp_start_response" in
    *'totp_required=1'*) ;;
    *)
      printf '%s\n' "Packed TOTP auth did not require TOTP" >&2
      printf '%s\n' "$totp_start_response" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-totp-no-enrollment.headers"
  totp_no_enrollment_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o "$work_dir/packed-totp-no-enrollment.txt" -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&password=packed-password&totp_code=287082' \
    "http://127.0.0.1:$kore_packed_port/auth-totp/webdav-key")
  totp_no_enrollment_body=$(cat "$work_dir/packed-totp-no-enrollment.txt")
  if [ "$totp_no_enrollment_status" != "401" ]; then
    printf '%s\n' "Packed TOTP auth returned unexpected no-enrollment status: $totp_no_enrollment_status" >&2
    printf '%s\n' "$totp_no_enrollment_body" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed totp no enrollment"
  if [ "$totp_no_enrollment_body" != "login failed" ]; then
    printf '%s\n' "Packed TOTP auth returned unexpected no-enrollment body: $totp_no_enrollment_body" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-totp-webdav-key.headers"
  totp_key_response=$(curl_or_log "$packed_service_log" \
    "packed totp webdav key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-totp%40example.com&pending_transaction_id=$totp_pending_id&totp_code=287082" \
    "http://127.0.0.1:$kore_packed_port/auth-totp/webdav-key")
  assert_no_store_headers "$auth_headers" "packed totp webdav key"
  totp_client_id=$(printf '%s\n' "$totp_key_response" |
    sed -n 's/^client_id=//p')
  totp_client_secret=$(printf '%s\n' "$totp_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$totp_client_id" ] || [ -z "$totp_client_secret" ]; then
    printf '%s\n' "Packed TOTP auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$totp_key_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed totp guarded api" \
    --max-time 3 -fsS -u "$totp_client_id:$totp_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed TOTP guarded API response: $body" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-totp" "/auth-totp" \
    "$totp_client_id" "$totp_client_secret"
  auth_headers="$work_dir/packed-totp-continue-start.headers"
  totp_continue_start_response=$(curl_or_log "$packed_service_log" \
    "packed totp continue start" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-totp%40example.com&password=packed-password' \
    "http://127.0.0.1:$kore_packed_port/auth-totp/continue")
  assert_no_store_headers "$auth_headers" "packed totp continue start"
  totp_continue_pending_id=$(printf '%s\n' "$totp_continue_start_response" |
    sed -n 's/^pending_transaction_id=//p')
  if [ -z "$totp_continue_pending_id" ]; then
    printf '%s\n' "Packed TOTP continue auth did not return a pending transaction" >&2
    printf '%s\n' "$totp_continue_start_response" >&2
    return 1
  fi
  case "$totp_continue_start_response" in
    *'totp_required=1'*) ;;
    *)
      printf '%s\n' "Packed TOTP continue auth did not require TOTP" >&2
      printf '%s\n' "$totp_continue_start_response" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-totp-continue-key.headers"
  totp_continue_response=$(curl_or_log "$packed_service_log" \
    "packed totp continue key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-totp%40example.com&pending_transaction_id=$totp_continue_pending_id&totp_code=287082" \
    "http://127.0.0.1:$kore_packed_port/auth-totp/continue")
  assert_no_store_headers "$auth_headers" "packed totp continue key"
  totp_continue_client_id=$(printf '%s\n' "$totp_continue_response" |
    sed -n 's/^client_id=//p')
  totp_continue_client_secret=$(printf '%s\n' "$totp_continue_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$totp_continue_client_id" ] ||
      [ -z "$totp_continue_client_secret" ]; then
    printf '%s\n' "Packed TOTP continue auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$totp_continue_response" >&2
    return 1
  fi
  assert_packed_key_webdav_logout "packed-totp-continue" "/auth-totp" \
    "$totp_continue_client_id" "$totp_continue_client_secret"
  auth_headers="$work_dir/packed-limited-email-token.headers"
  limited_token_response=$(curl_or_log "$packed_service_log" \
    "packed limited email token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-limited/email-token")
  assert_no_store_headers "$auth_headers" "packed limited email token"
  limited_transaction_id=$(printf '%s\n' "$limited_token_response" |
    sed -n 's/^transaction_id=//p')
  limited_token=$(printf '%s\n' "$limited_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$limited_transaction_id" ] || [ -z "$limited_token" ]; then
    printf '%s\n' "Packed limited auth did not expose an email token" >&2
    printf '%s\n' "$limited_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-limited-wrong-one.headers"
  limited_wrong_one_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$limited_transaction_id&email_token=wrong-one" \
    "http://127.0.0.1:$kore_packed_port/auth-limited/webdav-key")
  if [ "$limited_wrong_one_status" != "401" ]; then
    printf '%s\n' "Packed limited auth first wrong token returned unexpected status: $limited_wrong_one_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed limited first wrong token"
  auth_headers="$work_dir/packed-limited-wrong-two.headers"
  limited_wrong_two_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$limited_transaction_id&email_token=wrong-two" \
    "http://127.0.0.1:$kore_packed_port/auth-limited/webdav-key")
  if [ "$limited_wrong_two_status" != "401" ]; then
    printf '%s\n' "Packed limited auth second wrong token returned unexpected status: $limited_wrong_two_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed limited second wrong token"
  auth_headers="$work_dir/packed-limited-after-budget.headers"
  limited_after_budget_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$limited_transaction_id&email_token=$limited_token" \
    "http://127.0.0.1:$kore_packed_port/auth-limited/webdav-key")
  if [ "$limited_after_budget_status" != "401" ]; then
    printf '%s\n' "Packed limited auth accepted token after attempt budget: $limited_after_budget_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed limited after budget"
  auth_headers="$work_dir/packed-expired-email-token.headers"
  expired_token_response=$(curl_or_log "$packed_service_log" \
    "packed expired email token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&email=packed-email-only%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  assert_no_store_headers "$auth_headers" "packed expired email token"
  expired_transaction_id=$(printf '%s\n' "$expired_token_response" |
    sed -n 's/^transaction_id=//p')
  expired_token=$(printf '%s\n' "$expired_token_response" |
    sed -n 's/^token=//p')
  case "$expired_token_response" in
    *'expires_at=359'*) ;;
    *)
      printf '%s\n' "Packed local email token did not use expected expiry" >&2
      printf '%s\n' "$expired_token_response" >&2
      return 1
      ;;
  esac
  if [ -z "$expired_transaction_id" ] || [ -z "$expired_token" ]; then
    printf '%s\n' "Packed local auth did not expose a test email token" >&2
    printf '%s\n' "$expired_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-expired-token-webdav-key.headers"
  expired_token_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&password=packed-password&email_transaction_id=$expired_transaction_id&email_token=$expired_token" \
    "http://127.0.0.1:$kore_packed_port/auth-expired/webdav-key")
  if [ "$expired_token_status" != "401" ]; then
    printf '%s\n' "Packed auth expired email token returned unexpected status: $expired_token_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed expired token webdav key"
  auth_headers="$work_dir/packed-expired-token-replay.headers"
  expired_token_replay_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&password=packed-password&email_transaction_id=$expired_transaction_id&email_token=$expired_token" \
    "http://127.0.0.1:$kore_packed_port/auth-expired/webdav-key")
  if [ "$expired_token_replay_status" != "401" ]; then
    printf '%s\n' "Packed auth expired email token replay returned unexpected status: $expired_token_replay_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed expired token replay"
  auth_headers="$work_dir/packed-blocked-email-token.headers"
  blocked_email_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40blocked.test' \
    "http://127.0.0.1:$kore_packed_port/auth/email-token")
  if [ "$blocked_email_status" != "400" ]; then
    printf '%s\n' "Packed auth allowlisted SMTP route returned unexpected blocked-email status: $blocked_email_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed blocked email token"
  if [ -e "$packed_service_mailbox" ]; then
    printf '%s\n' "Packed SMTP harness mailbox was written for blocked recipient" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-smtp-email-token.headers"
  email_token_response=$(curl_or_log "$packed_service_log" \
    "packed smtp email token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&email=packed-user%40example.test&pending_transaction_id=$browser_pending_id" \
    "http://127.0.0.1:$kore_packed_port/auth/email-token")
  assert_no_store_headers "$auth_headers" "packed smtp email token"
  email_transaction_id=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^transaction_id=//p')
  if [ -z "$email_transaction_id" ]; then
    printf '%s\n' "Packed auth did not issue an email transaction" >&2
    printf '%s\n' "$email_token_response" >&2
    return 1
  fi
  assert_packed_auth_state_record "packed smtp email token" \
    "$email_transaction_id"
  case "$email_token_response" in
    *'token='*)
      printf '%s\n' "Packed auth exposed SMTP-delivered token in HTTP response" >&2
      printf '%s\n' "$email_token_response" >&2
      return 1
      ;;
  esac
  if [ ! -s "$packed_service_mailbox" ]; then
    printf '%s\n' "Packed SMTP harness did not capture a mailbox" >&2
    return 1
  fi
  mail_body=$(cat "$packed_service_mailbox")
  case "$mail_body" in
    *'Subject: Vectis login token'*"Transaction: $email_transaction_id"*) ;;
    *)
      printf '%s\n' "Packed SMTP mailbox did not contain expected token email" >&2
      printf '%s\n' "$mail_body" >&2
      return 1
      ;;
  esac
  email_token=$(printf '%s\n' "$mail_body" |
    sed -n 's/.*Your Vectis login token is: \([^[:space:]]*\).*/\1/p')
  if [ -z "$email_token" ]; then
    printf '%s\n' "Packed SMTP mailbox did not contain a login token" >&2
    printf '%s\n' "$mail_body" >&2
    return 1
  fi
  webdav_key_headers="$work_dir/packed-browser-continue-webdav-key.headers"
  webdav_key_response=$(curl_or_log "$packed_service_log" \
    "packed browser continue webdav key" --max-time 3 -fsS -D "$webdav_key_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&pending_transaction_id=$browser_pending_id&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth/continue")
  assert_no_store_headers "$webdav_key_headers" "packed browser continue webdav key"
  auth_headers="$work_dir/packed-replay-email-token.headers"
  replay_token_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&pending_transaction_id=$browser_pending_id&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth/continue")
  if [ "$replay_token_status" != "401" ]; then
    printf '%s\n' "Packed auth replayed email token returned unexpected status: $replay_token_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed replayed email token"
  webdav_client_id=$(printf '%s\n' "$webdav_key_response" |
    sed -n 's/^client_id=//p')
  webdav_client_secret=$(printf '%s\n' "$webdav_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$webdav_client_id" ] || [ -z "$webdav_client_secret" ]; then
    printf '%s\n' "Packed auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$webdav_key_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-local-missing-totp-token.headers"
  email_token_response=$(curl_or_log "$packed_service_log" \
    "packed local missing TOTP token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  assert_no_store_headers "$auth_headers" "packed local missing TOTP token"
  email_transaction_id=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^transaction_id=//p')
  email_token=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$email_transaction_id" ] || [ -z "$email_token" ]; then
    printf '%s\n' "Packed local auth did not expose a missing-TOTP token" >&2
    printf '%s\n' "$email_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-missing-totp.headers"
  missing_totp_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o "$work_dir/packed-missing-totp.txt" -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-local/webdav-key")
  missing_totp_body=$(cat "$work_dir/packed-missing-totp.txt")
  if [ "$missing_totp_status" != "202" ]; then
    printf '%s\n' "Packed auth missing TOTP returned unexpected status: $missing_totp_status" >&2
    printf '%s\n' "$missing_totp_body" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed missing TOTP"
  case "$missing_totp_body" in
    *'totp_required=1'*) ;;
    *)
      printf '%s\n' "Packed auth missing TOTP did not request TOTP continuation" >&2
      printf '%s\n' "$missing_totp_body" >&2
      return 1
      ;;
  esac
  case "$missing_totp_body" in
    *client_id=*|*client_secret=*)
      printf '%s\n' "Packed auth missing TOTP exposed credentials" >&2
      return 1
      ;;
  esac
  auth_headers="$work_dir/packed-local-wrong-token.headers"
  email_token_response=$(curl_or_log "$packed_service_log" \
    "packed local wrong token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  assert_no_store_headers "$auth_headers" "packed local wrong token"
  email_transaction_id=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^transaction_id=//p')
  if [ -z "$email_transaction_id" ]; then
    printf '%s\n' "Packed local auth did not expose a wrong-token transaction" >&2
    printf '%s\n' "$email_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-wrong-email-token.headers"
  wrong_token_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=wrong" \
    "http://127.0.0.1:$kore_packed_port/auth-local/webdav-key")
  if [ "$wrong_token_status" != "401" ]; then
    printf '%s\n' "Packed auth wrong email token returned unexpected status: $wrong_token_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed wrong email token"
  auth_headers="$work_dir/packed-local-wrong-totp-token.headers"
  email_token_response=$(curl_or_log "$packed_service_log" \
    "packed local wrong TOTP token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  assert_no_store_headers "$auth_headers" "packed local wrong TOTP token"
  email_transaction_id=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^transaction_id=//p')
  email_token=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$email_transaction_id" ] || [ -z "$email_token" ]; then
    printf '%s\n' "Packed local auth did not expose a wrong-TOTP token" >&2
    printf '%s\n' "$email_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-wrong-totp.headers"
  wrong_totp_status=$(curl --max-time 3 -sS -D "$auth_headers" \
    -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=000000&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-local/webdav-key")
  if [ "$wrong_totp_status" != "401" ]; then
    printf '%s\n' "Packed auth wrong TOTP returned unexpected status: $wrong_totp_status" >&2
    return 1
  fi
  assert_no_store_headers "$auth_headers" "packed wrong TOTP"
  auth_headers="$work_dir/packed-local-all-factor-token.headers"
  email_token_response=$(curl_or_log "$packed_service_log" \
    "packed local all-factor token" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  assert_no_store_headers "$auth_headers" "packed local all-factor token"
  email_transaction_id=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^transaction_id=//p')
  email_token=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$email_transaction_id" ] || [ -z "$email_token" ]; then
    printf '%s\n' "Packed local auth did not expose an all-factor token" >&2
    printf '%s\n' "$email_token_response" >&2
    return 1
  fi
  auth_headers="$work_dir/packed-local-all-factor-webdav-key.headers"
  local_all_factor_key_response=$(curl_or_log "$packed_service_log" \
    "packed local all-factor webdav key" --max-time 3 -fsS -D "$auth_headers" -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-local/webdav-key")
  assert_no_store_headers "$auth_headers" "packed local all-factor webdav key"
  local_all_factor_client_id=$(printf '%s\n' "$local_all_factor_key_response" |
    sed -n 's/^client_id=//p')
  local_all_factor_client_secret=$(printf '%s\n' "$local_all_factor_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$local_all_factor_client_id" ] ||
      [ -z "$local_all_factor_client_secret" ]; then
    printf '%s\n' "Packed all-factor auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$local_all_factor_key_response" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed all-factor guarded api" \
    --max-time 3 -fsS \
    -u "$local_all_factor_client_id:$local_all_factor_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed all-factor guarded API response: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed all-factor webdav read" \
    --max-time 3 -fsS \
    -u "$local_all_factor_client_id:$local_all_factor_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed all-factor WebDAV body: $body" >&2
      return 1
      ;;
  esac
  unauth_dav_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$unauth_dav_status" = "200" ]; then
    printf '%s\n' "Packed WebDAV unexpectedly allowed unauthenticated GET" >&2
    return 1
  fi
  unauth_dav_put_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X PUT --data 'anonymous write' \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$unauth_dav_put_status" = "200" ] ||
      [ "$unauth_dav_put_status" = "201" ] ||
      [ "$unauth_dav_put_status" = "204" ]; then
    printf '%s\n' "Packed WebDAV unexpectedly allowed unauthenticated PUT: $unauth_dav_put_status" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Anonymous packed WebDAV PUT mutated extracted docroot: $body" >&2
      return 1
      ;;
  esac
  traversal_status=$(curl --path-as-is --max-time 3 -sS -o /dev/null \
    -w '%{http_code}' -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/../index.html" || true)
  if [ "$traversal_status" != "400" ] &&
      [ "$traversal_status" != "404" ]; then
    printf '%s\n' "Unexpected packed WebDAV traversal GET status: $traversal_status" >&2
    return 1
  fi
  traversal_status=$(curl --path-as-is --max-time 3 -sS -o /dev/null \
    -w '%{http_code}' -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'escaped write' \
    "http://127.0.0.1:$kore_packed_port/dav/../webdav-escape.txt" || true)
  if [ "$traversal_status" != "400" ] &&
      [ "$traversal_status" != "404" ]; then
    printf '%s\n' "Unexpected packed WebDAV traversal PUT status: $traversal_status" >&2
    return 1
  fi
  packed_service_docroot_parent=$(dirname "$packed_service_docroot")
  if [ -e "$packed_service_docroot_parent/webdav-escape.txt" ] ||
      [ -e "$packed_service_docroot/webdav-escape.txt" ]; then
    printf '%s\n' "Packed WebDAV traversal PUT created escaped file" >&2
    return 1
  fi
  guarded_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$guarded_status" = "200" ]; then
    printf '%s\n' "Packed guarded API unexpectedly allowed anonymous GET" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed guarded api" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed guarded API response: $body" >&2
    return 1
  fi
  printf '%s\n' \
    'local lockdc = require("lockdc")' \
    'local endpoint = assert(os.getenv("LOCKD_ENDPOINT"))' \
    'local bundle = assert(os.getenv("LOCKD_CLIENT_BUNDLE"))' \
    'local queue = assert(os.getenv("LOCKD_QUEUE"))' \
    'local namespace = assert(os.getenv("LOCKD_NAMESPACE"))' \
    'local client = assert(lockdc.open({' \
    '  endpoints = { endpoint },' \
    '  client_bundle_path = bundle,' \
    '  default_namespace = namespace,' \
    '}))' \
    'assert(client:enqueue({' \
    '  queue = queue,' \
    '  visibility_timeout_seconds = 30,' \
    '  ttl_seconds = 3600,' \
    '  max_attempts = 5,' \
    '  content_type = "text/plain",' \
    '}, "packed-consumer-message"))' \
    'client:close()' >"$packed_service_enqueue"
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_QUEUE="$packed_service_queue" \
    LOCKD_NAMESPACE="$packed_service_namespace" \
    "$repo_root/build/debug/vectis" "$packed_service_enqueue"
  count=0
  while :; do
    packed_consumer_body=$(curl --max-time 3 -fsS \
      -u "$webdav_client_id:$webdav_client_secret" \
      "http://127.0.0.1:$kore_packed_port/dav/consumer-processing.txt" || true)
    if [ "$packed_consumer_body" = "processing" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[lua-packed-webserver] /' "$packed_service_log" >&2
      printf '%s\n' "Packed consumer did not enter processing state: $packed_consumer_body" >&2
      return 1
    fi
    sleep 1
  done
  method_status=$(curl --max-time 5 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'webdav during packed consumer' \
    "http://127.0.0.1:$kore_packed_port/dav/live-during-consumer.txt")
  if [ "$method_status" != "201" ] && [ "$method_status" != "204" ]; then
    printf '%s\n' "Unexpected packed WebDAV PUT status during consumer work: $method_status" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed webdav live during consumer" \
    --max-time 5 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/live-during-consumer.txt")
  if [ "$body" != "webdav during packed consumer" ]; then
    printf '%s\n' "Unexpected packed WebDAV body during consumer work: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed webdav preserved file" \
    --max-time 5 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/user-created-before-repair.txt")
  if [ "$body" != "preexisting mutable content" ]; then
    printf '%s\n' "Unexpected packed WebDAV preserved file body: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed guarded api during consumer" \
    --max-time 5 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed guarded API body during consumer work: $body" >&2
    return 1
  fi
  count=0
  while :; do
    packed_consumer_body=$(curl --max-time 3 -fsS \
      -u "$webdav_client_id:$webdav_client_secret" \
      "http://127.0.0.1:$kore_packed_port/dav/consumer-done.txt" || true)
    if [ "$packed_consumer_body" = "handled" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[lua-packed-webserver] /' "$packed_service_log" >&2
      printf '%s\n' "Packed consumer did not finish: $packed_consumer_body" >&2
      return 1
    fi
    sleep 1
  done
  body=$(curl_or_log "$packed_service_log" "packed webdav index" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed WebDAV embedded response: $body" >&2
      return 1
      ;;
  esac
  propfind_status=$(curl --max-time 3 -sS -o "$work_dir/packed-propfind-root.xml" \
    -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PROPFIND -H 'Depth: 1' \
    "http://127.0.0.1:$kore_packed_port/dav")
  propfind_body=$(cat "$work_dir/packed-propfind-root.xml")
  if [ "$propfind_status" != "207" ]; then
    printf '%s\n' "Unexpected packed WebDAV PROPFIND status: $propfind_status" >&2
    printf '%s\n' "$propfind_body" >&2
    return 1
  fi
  case "$propfind_body" in
    *'<D:href>/dav/index.html</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list index.html: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/app.css</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list app.css: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/app.js</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list app.js: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/assets</D:href>'*|*'<D:href>/dav/assets/</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list assets directory: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/templates</D:href>'*|*'<D:href>/dav/templates/</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list templates directory: $propfind_body" >&2
      return 1
      ;;
  esac
  body=$(curl_or_log "$packed_service_log" "packed webdav logo" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Unexpected packed WebDAV logo response: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed webdav template" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/templates/login.html")
  case "$body" in
    *'packed-login'*'username'*) ;;
    *)
      printf '%s\n' "Unexpected packed WebDAV template response: $body" >&2
      return 1
      ;;
  esac
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X DELETE "http://127.0.0.1:$kore_packed_port/dav/assets/logo.txt")
  if [ "$method_status" != "204" ]; then
    printf '%s\n' "Unexpected packed WebDAV embedded asset DELETE status: $method_status" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/assets/logo.txt")
  if [ "$method_status" != "404" ]; then
    printf '%s\n' "Packed WebDAV embedded asset DELETE left logo readable: $method_status" >&2
    return 1
  fi
  if [ -e "$packed_service_docroot/assets/logo.txt" ]; then
    printf '%s\n' "Packed WebDAV embedded asset DELETE left extracted logo on disk" >&2
    return 1
  fi
  propfind_status=$(curl --max-time 3 -sS \
    -o "$work_dir/packed-propfind-assets-after-delete.xml" \
    -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PROPFIND -H 'Depth: 1' \
    "http://127.0.0.1:$kore_packed_port/dav/assets")
  propfind_body=$(cat "$work_dir/packed-propfind-assets-after-delete.xml")
  if [ "$propfind_status" != "207" ]; then
    printf '%s\n' "Unexpected packed WebDAV assets PROPFIND status after DELETE: $propfind_status" >&2
    printf '%s\n' "$propfind_body" >&2
    return 1
  fi
  case "$propfind_body" in
    *'<D:href>/dav/assets/logo.txt</D:href>'*)
      printf '%s\n' "Packed WebDAV PROPFIND listed deleted embedded asset: $propfind_body" >&2
      return 1
      ;;
  esac
  body=$(curl_or_log "$packed_service_log" "packed static logo after webdav delete" \
    --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Packed static embedded logo changed after WebDAV DELETE: $body" >&2
    return 1
  fi
  curl_or_log "$packed_service_log" "packed webdav index put" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'webdav index override' \
    "http://127.0.0.1:$kore_packed_port/dav/index.html" >/dev/null
  body=$(curl_or_log "$packed_service_log" "packed webdav index override" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$body" != "webdav index override" ]; then
    printf '%s\n' "Unexpected packed WebDAV index override: $body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/index.html")
  if [ "$body" != "webdav index override" ]; then
    printf '%s\n' "Packed WebDAV index override did not mutate extracted docroot: $body" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed static index after webdav override" \
    --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Packed static asset changed after WebDAV override: $body" >&2
      return 1
      ;;
  esac
  curl_or_log "$packed_service_log" "packed webdav note put" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'mutable packed note' \
    "http://127.0.0.1:$kore_packed_port/dav/note.txt" >/dev/null
  dav_write_body=$(curl_or_log "$packed_service_log" "packed webdav note" \
    --max-time 3 -fsS \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/note.txt")
  if [ "$dav_write_body" != "mutable packed note" ]; then
    printf '%s\n' "Unexpected packed WebDAV write response: $dav_write_body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/note.txt")
  if [ "$body" != "mutable packed note" ]; then
    printf '%s\n' "Packed WebDAV write did not land in extracted docroot: $body" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X MKCOL "http://127.0.0.1:$kore_packed_port/dav/docs")
  if [ "$method_status" != "201" ]; then
    printf '%s\n' "Unexpected packed WebDAV MKCOL status: $method_status" >&2
    return 1
  fi
  curl_or_log "$packed_service_log" "packed webdav docs a.txt put" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'copied then moved' \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a.txt" >/dev/null
  propfind_status=$(curl --max-time 3 -sS -o "$work_dir/packed-propfind-docs.xml" \
    -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PROPFIND -H 'Depth: 1' \
    "http://127.0.0.1:$kore_packed_port/dav/docs")
  propfind_body=$(cat "$work_dir/packed-propfind-docs.xml")
  if [ "$propfind_status" != "207" ]; then
    printf '%s\n' "Unexpected packed WebDAV collection PROPFIND status: $propfind_status" >&2
    printf '%s\n' "$propfind_body" >&2
    return 1
  fi
  case "$propfind_body" in
    *'<D:href>/dav/docs/a.txt</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV collection did not list a.txt: $propfind_body" >&2
      return 1
      ;;
  esac
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X COPY \
    -H "Destination: http://127.0.0.1:$kore_packed_port/dav/../copy-escape.txt" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a.txt")
  if [ "$method_status" != "400" ]; then
    printf '%s\n' "Unexpected packed WebDAV traversal COPY status: $method_status" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X MOVE \
    -H "Destination: http://127.0.0.1:$kore_packed_port/dav/../move-escape.txt" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a.txt")
  if [ "$method_status" != "400" ]; then
    printf '%s\n' "Unexpected packed WebDAV traversal MOVE status: $method_status" >&2
    return 1
  fi
  packed_service_docroot_parent=$(dirname "$packed_service_docroot")
  if [ -e "$packed_service_docroot_parent/copy-escape.txt" ] ||
      [ -e "$packed_service_docroot_parent/move-escape.txt" ] ||
      [ -e "$packed_service_docroot/copy-escape.txt" ] ||
      [ -e "$packed_service_docroot/move-escape.txt" ]; then
    printf '%s\n' "Packed WebDAV Destination traversal created escaped file" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X COPY \
    -H "Destination: http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a.txt")
  if [ "$method_status" != "201" ]; then
    printf '%s\n' "Unexpected packed WebDAV COPY status: $method_status" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed webdav copied body" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt")
  if [ "$body" != "copied then moved" ]; then
    printf '%s\n' "Unexpected packed WebDAV copied body: $body" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X MOVE \
    -H "Destination: http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt")
  if [ "$method_status" != "201" ]; then
    printf '%s\n' "Unexpected packed WebDAV MOVE status: $method_status" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt")
  if [ "$method_status" != "404" ]; then
    printf '%s\n' "Packed WebDAV MOVE left source readable: $method_status" >&2
    return 1
  fi
  body=$(curl_or_log "$packed_service_log" "packed webdav moved body" \
    --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt")
  if [ "$body" != "copied then moved" ]; then
    printf '%s\n' "Unexpected packed WebDAV moved body: $body" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X DELETE "http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt")
  if [ "$method_status" != "204" ]; then
    printf '%s\n' "Unexpected packed WebDAV DELETE status: $method_status" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt")
  if [ "$method_status" != "404" ]; then
    printf '%s\n' "Packed WebDAV DELETE left file readable: $method_status" >&2
    return 1
  fi
  logout_headers="$work_dir/packed-logout.headers"
  logout_status=$(curl --max-time 3 -sS -D "$logout_headers" \
    -o "$work_dir/packed-logout.txt" -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X POST -H 'Content-Type: application/x-www-form-urlencoded' --data '' \
    "http://127.0.0.1:$kore_packed_port/auth/logout")
  logout_body=$(cat "$work_dir/packed-logout.txt")
  if [ "$logout_status" != "200" ]; then
    printf '%s\n' "Packed auth logout returned unexpected status: $logout_status" >&2
    printf '%s\n' "$logout_body" >&2
    return 1
  fi
  assert_no_store_headers "$logout_headers" "packed logout"
  if [ "$logout_body" != "logged_out=1" ]; then
    printf '%s\n' "Packed auth logout returned unexpected body: $logout_body" >&2
    return 1
  fi
  logged_out_api_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$logged_out_api_status" != "401" ]; then
    printf '%s\n' "Packed auth logout left guarded API credential active: $logged_out_api_status" >&2
    return 1
  fi
  logged_out_dav_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$logged_out_dav_status" != "401" ]; then
    printf '%s\n' "Packed auth logout left WebDAV credential active: $logged_out_dav_status" >&2
    return 1
  fi
  logout_headers="$work_dir/packed-all-factor-logout.headers"
  logout_status=$(curl --max-time 3 -sS -D "$logout_headers" \
    -o "$work_dir/packed-all-factor-logout.txt" -w '%{http_code}' \
    -u "$local_all_factor_client_id:$local_all_factor_client_secret" \
    -X POST -H 'Content-Type: application/x-www-form-urlencoded' --data '' \
    "http://127.0.0.1:$kore_packed_port/auth-local/logout")
  logout_body=$(cat "$work_dir/packed-all-factor-logout.txt")
  if [ "$logout_status" != "200" ]; then
    printf '%s\n' "Packed all-factor logout returned unexpected status: $logout_status" >&2
    printf '%s\n' "$logout_body" >&2
    return 1
  fi
  assert_no_store_headers "$logout_headers" "packed all-factor logout"
  if [ "$logout_body" != "logged_out=1" ]; then
    printf '%s\n' "Packed all-factor logout returned unexpected body: $logout_body" >&2
    return 1
  fi
  logged_out_api_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$local_all_factor_client_id:$local_all_factor_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$logged_out_api_status" != "401" ]; then
    printf '%s\n' "Packed all-factor logout left guarded API credential active: $logged_out_api_status" >&2
    return 1
  fi
  logged_out_dav_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$local_all_factor_client_id:$local_all_factor_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$logged_out_dav_status" != "401" ]; then
    printf '%s\n' "Packed all-factor logout left WebDAV credential active: $logged_out_dav_status" >&2
    return 1
  fi

  printf '[e2e] lua libmdf example\n'
  "$repo_root/build/debug/vectis" "$repo_root/examples/lua/mdf_render.lua"
}

run_tls_cert_examples() {
  printf '[e2e] certificate generation and validation\n'
  "$repo_root/build/debug/tests/unit/vectis_unit_certs"

  printf '[e2e] https runtime with CA chain validation\n'
  env VECTIS_TEST_HTTPS_PORT="$https_runtime_port" \
    "$repo_root/build/debug/tests/unit/vectis_unit_https_runtime"

  printf '[e2e] https runtime with required client certificate\n'
  env VECTIS_TEST_HTTPS_MTLS_PORT="$https_mtls_runtime_port" \
    "$repo_root/build/debug/tests/unit/vectis_unit_https_mtls_runtime"
}

run_kore_examples() {
  body=

  printf '[e2e] kore basic server\n'
  start_server "kore basic" "$work_dir/kore-basic.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_basic_port" \
      "$repo_root/build/debug/examples/vectis_example_kore_basic"
  wait_for_http "http://127.0.0.1:$kore_basic_port/health" "kore basic server"
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_basic_port/health")
  if [ "$body" != "ok" ]; then
    printf '%s\n' "Unexpected /health response: $body" >&2
    return 1
  fi

  printf '[e2e] kore lockd api\n'
  start_server "kore lockd" "$work_dir/kore-lockd.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_lockd_port" \
      LOCKD_ENDPOINT="$disk_endpoint" \
      LOCKD_CLIENT_BUNDLE="$client_bundle" \
      "$repo_root/build/debug/examples/vectis_example_kore_lockd_api"
  wait_for_http "http://127.0.0.1:$kore_lockd_port/state/e2e-order" "kore lockd api"
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_lockd_port/state/e2e-order")
  case "$body" in
    *'"id":"e2e-order"'*'"status":"loaded"'*) ;;
    *)
      printf '%s\n' "Unexpected /state response: $body" >&2
      return 1
      ;;
  esac

  printf '[e2e] kore lockd workflow with consumer deferral\n'
  workflow_id=${VECTIS_E2E_WORKFLOW_ID:-"e2e-$$"}
  workflow_queue=${VECTIS_E2E_WORKFLOW_QUEUE:-"vectis-e2e-workflow-$$"}
  workflow_namespace=${VECTIS_E2E_WORKFLOW_NAMESPACE:-"examples"}
  workflow_content=${VECTIS_E2E_WORKFLOW_CONTENT:-"vectis workflow content $$"}
  workflow_consumer_first_log="$work_dir/workflow-consumer-first.log"
  workflow_consumer_second_log="$work_dir/workflow-consumer-second.log"
  start_server "kore workflow" "$work_dir/kore-workflow.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_workflow_port" \
      LOCKD_ENDPOINT="$disk_endpoint" \
      LOCKD_CLIENT_BUNDLE="$client_bundle" \
      VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
      VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
      VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
      VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
      "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" server
  wait_for_http "http://127.0.0.1:$kore_workflow_port/health" "kore workflow api"
  body=$(curl --max-time 5 -fsS \
    -H 'content-type: application/json' \
    -X POST \
    --data "{\"content\":\"$workflow_content\"}" \
    "http://127.0.0.1:$kore_workflow_port/workflow/$workflow_id")
  case "$body" in
    *"\"id\":\"$workflow_id\""*"\"queue\":\"$workflow_queue\""*'"counter":1'*) ;;
    *)
      printf '%s\n' "Unexpected workflow response: $body" >&2
      return 1
      ;;
  esac
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
    VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
    VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
    VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
    "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" consumer-second \
      >"$workflow_consumer_second_log" 2>&1 &
  workflow_consumer_second_pid=$!
  server_pids="$server_pids $workflow_consumer_second_pid"
  count=0
  while ! grep -q 'workflow.consumer.not_ready' "$workflow_consumer_second_log"; do
    count=$((count + 1))
    if [ "$count" -ge 20 ]; then
      sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
      printf '%s\n' "workflow second consumer did not defer the not-ready message" >&2
      return 1
    fi
    if ! kill -0 "$workflow_consumer_second_pid" >/dev/null 2>&1; then
      sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
      printf '%s\n' "workflow second consumer exited before first deferral" >&2
      return 1
    fi
    sleep 1
  done
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
    VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
    VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
    VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
    "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" consumer-first \
      >"$workflow_consumer_first_log" 2>&1 &
  workflow_consumer_first_pid=$!
  server_pids="$server_pids $workflow_consumer_first_pid"
  if ! wait "$workflow_consumer_first_pid"; then
    sed 's/^/[workflow-consumer-first] /' "$workflow_consumer_first_log" >&2
    return 1
  fi
  if ! wait "$workflow_consumer_second_pid"; then
    sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
    return 1
  fi
  if ! grep -q 'workflow.consumer.defer' "$workflow_consumer_first_log"; then
    sed 's/^/[workflow-consumer-first] /' "$workflow_consumer_first_log" >&2
    printf '%s\n' "workflow first consumer did not defer after incrementing counter" >&2
    return 1
  fi
  if ! grep -q 'workflow.consumer.ack' "$workflow_consumer_second_log"; then
    sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
    printf '%s\n' "workflow second consumer did not ack final message" >&2
    return 1
  fi
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
    VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
    VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
    VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
    "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" verify

  printf '[e2e] kore webdav and lockd consumer in one process\n'
  combined_queue=${VECTIS_E2E_COMBINED_QUEUE:-"vectis-e2e-combined-$$"}
  combined_namespace=${VECTIS_E2E_COMBINED_NAMESPACE:-"examples"}
  combined_cache="$work_dir/combined-webdav"
  start_server "kore webdav lockd consumer" "$work_dir/kore-combined.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_combined_port" \
      LOCKD_ENDPOINT="$disk_endpoint" \
      LOCKD_CLIENT_BUNDLE="$client_bundle" \
      VECTIS_E2E_COMBINED_QUEUE="$combined_queue" \
      VECTIS_E2E_COMBINED_NAMESPACE="$combined_namespace" \
      VECTIS_E2E_COMBINED_WEBDAV_CACHE="$combined_cache" \
      "$repo_root/build/debug/examples/vectis_example_kore_webdav_lockd_consumer_e2e"
  wait_for_http "http://127.0.0.1:$kore_combined_port/health" "kore webdav lockd consumer"
  body=$(curl --max-time 5 -fsS -X POST --data '' \
    "http://127.0.0.1:$kore_combined_port/enqueue/e2e-combined-$$")
  if [ "$body" != "queued" ]; then
    printf '%s\n' "Unexpected combined enqueue response: $body" >&2
    return 1
  fi
  count=0
  while :; do
    body=$(curl --max-time 3 -fsS \
      "http://127.0.0.1:$kore_combined_port/dav/consumer-processing.txt" || true)
    if [ "$body" = "processing" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[kore-combined] /' "$work_dir/kore-combined.log" >&2
      printf '%s\n' "combined consumer did not enter processing state: $body" >&2
      return 1
    fi
    sleep 1
  done
  status=$(printf 'during consumer\n' | curl --max-time 5 -fsS \
    -o /dev/null -w '%{http_code}' -X PUT --data-binary @- \
    "http://127.0.0.1:$kore_combined_port/dav/live.txt")
  if [ "$status" != "201" ] && [ "$status" != "204" ]; then
    printf '%s\n' "Unexpected combined WebDAV PUT status during consumer work: $status" >&2
    return 1
  fi
  body=$(curl --max-time 5 -fsS \
    "http://127.0.0.1:$kore_combined_port/dav/live.txt")
  if [ "$body" != "during consumer" ]; then
    printf '%s\n' "Unexpected combined WebDAV body during consumer work: $body" >&2
    return 1
  fi
  body=$(curl --max-time 5 -fsS \
    "http://127.0.0.1:$kore_combined_port/health")
  if [ "$body" != "ok" ]; then
    printf '%s\n' "Unexpected combined API health body during consumer work: $body" >&2
    return 1
  fi
  count=0
  while :; do
    body=$(curl --max-time 3 -fsS \
      "http://127.0.0.1:$kore_combined_port/dav/consumer-done.txt" || true)
    if [ "$body" = "handled" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[kore-combined] /' "$work_dir/kore-combined.log" >&2
      printf '%s\n' "combined consumer did not finish: $body" >&2
      return 1
    fi
    sleep 1
  done
}

provision_ssh_public_key() {
  if ! command -v ssh-keygen >/dev/null 2>&1 || ! command -v ssh-keyscan >/dev/null 2>&1; then
    printf '%s\n' "ssh-keygen and ssh-keyscan are required for SSH e2e coverage" >&2
    return 1
  fi
  ssh-keygen -q -t rsa -b 2048 -m PEM -N "" -f "$ssh_memory_key"
  ssh-keygen -q -t rsa -b 2048 -m PEM -N "" -f "$ssh_bad_host_key"
}

install_ssh_public_key() {
  "$script_dir/compose.sh" exec -T ssh-sftp /bin/sh -ec \
    'mkdir -p /config/.ssh && cat > /config/.ssh/authorized_keys && chmod 700 /config/.ssh && chmod 600 /config/.ssh/authorized_keys' \
    <"$ssh_memory_key.pub"
}

provision_ssh_known_hosts() {
  count=0
  while ! ssh-keyscan -p "$ssh_port" -T 5 127.0.0.1 >"$ssh_known_hosts" 2>/dev/null; do
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      printf '%s\n' "failed to scan SSH/SFTP host key" >&2
      return 1
    fi
    sleep 1
  done
  if [ ! -s "$ssh_known_hosts" ]; then
    printf '%s\n' "ssh-keyscan returned an empty known_hosts file" >&2
    return 1
  fi
  printf '[127.0.0.1]:%s %s\n' "$ssh_port" "$(cat "$ssh_bad_host_key.pub")" >"$ssh_bad_known_hosts"
  ssh_host_key_sha256=$(
    ssh-keygen -lf "$ssh_known_hosts" -E sha256 |
      awk '{print $2}' |
      paste -sd, -
  )
  if [ -z "$ssh_host_key_sha256" ]; then
    printf '%s\n' "failed to derive SSH host-key SHA-256 fingerprints" >&2
    return 1
  fi
}

"$script_dir/dev-reset.sh"
provision_ssh_public_key
"$script_dir/dev-up.sh"
install_ssh_public_key
provision_ssh_known_hosts
make -C "$repo_root" build-debug

cd "$work_dir"
run_lua_examples
run_tls_cert_examples
run_lockd_examples "$disk_endpoint" disk
run_lockd_examples "$s3_endpoint" s3
run_lockd_lua_examples "$disk_endpoint" disk
run_lockd_lua_examples "$s3_endpoint" s3
run_lockd_lua_consumer_example "$disk_endpoint" disk
run_lockd_failure_examples
run_service_examples
run_downstream_http_examples
run_static_asset_examples
run_kore_examples
